// SPDX-License-Identifier: GPL-2.0
/*
 * hd60pro - experimental Linux driver for the Elgato Game Capture HD60 Pro
 *           (YUAN MZ0380 / VATICS "Mozart" PCIe capture card, 12ab:0380/0381)
 *
 * Written from a clean-room analysis of the protocol used by the Windows
 * driver (e60MZ0380.X64.SYS 1.1.0.194) and the card-side endpoint module
 * (ep.ko) that ships inside the card's own firmware package.
 *
 * Protocol summary (BAR0 = 32 MiB window into card RAM, BAR5 = 4 KiB card
 * PCIe controller registers):
 *
 *   BAR0+0x00  doorbell: 0x800 = "command posted", 0x400 = "irq acknowledged"
 *   BAR0+0x04  command id
 *   BAR0+0x08.. command parameters (up to 9 dwords) / response data
 *   BAR0+0x2c  command acknowledge word written by the card (host clears to 0)
 *   BAR0+0x30  interrupt status (bit0..7 video ch, bit11 cmd done,
 *              bit16..23 audio ch); host clears to 0 after reading
 *   BAR0+0x40/44/48  video "channel done" info, one nibble per channel
 *   BAR0+0x4c  audio "channel done" info, one nibble per channel
 *   BAR5+0xdc  write 2 to de-assert the legacy interrupt
 *   BAR5+0x30/0x38 inbound window start/limit (BAR0 bus address +4/+0x5f)
 *
 * The card runs embedded Linux from its own flash.  This driver NEVER
 * uploads or flashes firmware; firmware-download commands are blocked.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

#define DRV_NAME	"hd60pro"
#define DRV_VERSION	"0.7.0"

/* ---- register map ------------------------------------------------------ */
#define R0_DOORBELL	0x00
#define R0_CMD		0x04
#define R0_PARAM(i)	(0x08 + 4 * (i))
#define R0_ACK		0x2c
#define R0_STATUS	0x30
#define R0_VDONE0	0x40
#define R0_VDONE1	0x44
#define R0_VDONE2	0x48
#define R0_ADONE	0x4c

#define R5_INTX_CLR	0xdc
#define R5_IB_START	0x30
#define R5_IB_LIMIT	0x38

#define DB_CMD		0x800
#define DB_IRQ_ACK	0x400

#define ST_CMD_DONE	BIT(11)
#define ST_VIDEO_MASK	0x000000ff
#define ST_AUDIO_MASK	0x00ff0000
#define ST_AUDIO_BUFS	0x000f0000	/* bit 16+n: channel 0 audio buffer n is full */

/* ---- command ids ------------------------------------------------------- */
#define CMD_FW_READY	0x01
#define CMD_VBUF	0x02	/* register preview (uncompressed) buffers  */
#define CMD_ABUF	0x03	/* register audio buffers                    */
#define CMD_EBUF_MAIN	0x04	/* register H.264 main-stream buffers        */
#define CMD_EBUF_SUB	0x05	/* register H.264 sub-stream buffers         */
#define CMD_START	0x06
#define CMD_STOP	0x07
#define CMD_FW_VERSION	0x0a
#define CMD_SET_VIC	0x29
#define CMD_SET_AIC	0x2a

#define ACK_FW_READY	0xdddddddd
#define ACK_VER_OK	0xaaaaaaaa

#define NUM_VBUF	4
#define VBUF_SIZE	0x466000	/* what Windows registers for this board */
#define EBUF_SIZE	0x34bd00
#define ABUF_CHUNK	0x2000
#define ABUF_CHANNELS	8
#define ABUF_PER_CH	4

/* ---- module parameters ------------------------------------------------- */
static int pixmode;		/* 0 = auto, 6 = YUY2, 5/7 = YV12 */
module_param(pixmode, int, 0644);
MODULE_PARM_DESC(pixmode, "Card output mode: 0=auto, 6=YUY2 (4:2:2), 5 or 7=YV12 (4:2:0)");

static bool use_msi = true;
module_param(use_msi, bool, 0444);
MODULE_PARM_DESC(use_msi, "Use MSI interrupts (default y); n = legacy INTx");

static bool poll;
module_param(poll, bool, 0444);
MODULE_PARM_DESC(poll, "Poll the status register every 1 ms instead of using interrupts");

static bool reg_h264 = true;
module_param(reg_h264, bool, 0444);
MODULE_PARM_DESC(reg_h264, "Register H.264 buffers with the card like the Windows driver does");

static int force_width, force_height, force_fps;
module_param(force_width, int, 0644);
module_param(force_height, int, 0644);
module_param(force_fps, int, 0644);
MODULE_PARM_DESC(force_width, "Override detected input width");
MODULE_PARM_DESC(force_height, "Override detected input height");
MODULE_PARM_DESC(force_fps, "Override detected input frame rate");

static bool mst_init = true;
module_param(mst_init, bool, 0444);
MODULE_PARM_DESC(mst_init, "Initialise the MST3367 HDMI receiver at load time (default y)");

static bool uv_swap;
module_param(uv_swap, bool, 0644);
MODULE_PARM_DESC(uv_swap, "Swap U/V planes in 4:2:0 mode (set if colours are wrong)");

static int y_align = 1;
module_param(y_align, int, 0644);
MODULE_PARM_DESC(y_align, "Row alignment of the luma plane height in 4:2:0 mode (1, 2, 8 or 16)");

static int bitstreams;
module_param(bitstreams, int, 0644);
MODULE_PARM_DESC(bitstreams, "H.264 streams the card encodes alongside the preview (default 0; 1 = like Windows, but limits capture to ~55 fps)");

static int audio_chunk = 4096;
module_param(audio_chunk, int, 0644);
MODULE_PARM_DESC(audio_chunk, "Valid bytes per card audio buffer (default 4096 = 1024 stereo frames)");

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug level (0-3)");

#define dbg(lvl, d, fmt, ...) do { if (debug >= (lvl)) \
	dev_info(&(d)->pdev->dev, fmt, ##__VA_ARGS__); } while (0)

/* ---- driver state ------------------------------------------------------ */
struct hd60_dmabuf {
	void		*cpu;
	dma_addr_t	dma;
	size_t		size;
};

struct hd60_buffer {
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
};

struct hd60_dev {
	struct pci_dev		*pdev;
	void __iomem		*bar0;
	void __iomem		*bar5;
	resource_size_t		bar0_bus;
	int			irq;
	bool			irq_ok;
	struct task_struct	*poll_thread;

	struct mutex		cmd_lock;
	spinlock_t		reg_lock;
	wait_queue_head_t	cmd_wq;
	atomic_t		cmd_done;
	u32			fw_major, fw_minor;
	bool			hw_ready;
	u8			mst_page;

	struct hd60_dmabuf	vbuf[NUM_VBUF];
	struct hd60_dmabuf	ebuf_main[NUM_VBUF];
	struct hd60_dmabuf	ebuf_sub[NUM_VBUF];
	struct hd60_dmabuf	abuf;

	/* pending events from the hard irq to the thread */
	spinlock_t		evt_lock;
	u32			evt_status;
	u32			evt_vdone[3];
	u32			evt_adone;
	unsigned long		irq_count, frame_irqs, frames_out, frames_dropped;
	int			last_vidx;

	/* signal */
	u32			sig_w, sig_h, sig_fps;
	bool			sig_interlaced, sig_valid;
	u8			sig_raw[0x20];
	unsigned long		sig_jiffies;

	/* v4l2 */
	struct v4l2_device	v4l2_dev;
	struct video_device	vdev;
	struct vb2_queue	queue;
	struct mutex		lock;
	spinlock_t		qlock;
	struct list_head	buf_list;
	u32			sequence;
	bool			streaming;
	ktime_t			stream_start, first_frame, last_frame;
	u32			width, height, fps, pixfmt, cardmode;
	u32			bytesperline, sizeimage;

	struct dentry		*dbg_dir;

	/* shared hardware start/stop between video and audio */
	struct mutex		hw_lock;
	unsigned int		hw_users;
#define HW_VIDEO	BIT(0)
#define HW_AUDIO	BIT(1)

	/* ALSA */
	struct snd_card		*card;
	struct snd_pcm		*pcm;
	struct snd_pcm_substream *asub;
	spinlock_t		alock;
	bool			a_running;
	size_t			a_pos, a_period_acc;
	unsigned long		audio_irqs, audio_multi;
	int			last_aidx;
	struct { u64 t; u32 st; u16 mask, pos; } atrace[512];
	unsigned int		atrace_n;
	unsigned long		a_copies;
	u32			dbg_last_resp[10];
	int			dbg_last_ret;
	u32			dbg_last_ack;
};

static inline u32 r0(struct hd60_dev *d, u32 off) { return ioread32(d->bar0 + off); }
static inline void w0(struct hd60_dev *d, u32 off, u32 v) { iowrite32(v, d->bar0 + off); }
static inline void w5(struct hd60_dev *d, u32 off, u32 v) { iowrite32(v, d->bar5 + off); }

/* ======================================================================== */
/* interrupt / status handling                                              */
/* ======================================================================== */

/* Read and acknowledge the status register.  Returns the status (0 = none). */
static u32 hd60_ack_status(struct hd60_dev *d)
{
	unsigned long flags;
	u32 st;

	spin_lock_irqsave(&d->reg_lock, flags);
	st = r0(d, R0_STATUS);
	if (st == 0 || st == 0xffffffff) {
		spin_unlock_irqrestore(&d->reg_lock, flags);
		return 0;
	}
	w5(d, R5_INTX_CLR, 2);
	w0(d, R0_STATUS, 0);
	w0(d, R0_DOORBELL, DB_IRQ_ACK);

	spin_lock(&d->evt_lock);
	d->evt_status |= st;
	if (st & ST_VIDEO_MASK) {
		d->evt_vdone[0] = r0(d, R0_VDONE0);
		d->evt_vdone[1] = r0(d, R0_VDONE1);
		d->evt_vdone[2] = r0(d, R0_VDONE2);
		/* tell the card the frame was taken (Windows ISR does this too);
		 * without it the card only sends a frame every 100 ms */
		iowrite8(0, d->bar0 + 0x50);
	}
	if (st & ST_AUDIO_MASK)
		d->evt_adone = r0(d, R0_ADONE);
	spin_unlock(&d->evt_lock);
	spin_unlock_irqrestore(&d->reg_lock, flags);

	d->irq_count++;
	if (st & ST_CMD_DONE) {
		atomic_set(&d->cmd_done, 1);
		wake_up(&d->cmd_wq);
	}
	return st;
}

static void hd60_deliver_frame(struct hd60_dev *d, int idx);
static void hd60_audio_irq(struct hd60_dev *d, u32 mask);

/* Process pending events (thread context). */
static void hd60_process_events(struct hd60_dev *d)
{
	unsigned long flags;
	u32 st, v0, v1, v2;

	spin_lock_irqsave(&d->evt_lock, flags);
	st = d->evt_status;
	v0 = d->evt_vdone[0];
	v1 = d->evt_vdone[1];
	v2 = d->evt_vdone[2];
	d->evt_status = 0;
	spin_unlock_irqrestore(&d->evt_lock, flags);

	if (!st)
		return;

	if (st & ST_VIDEO_MASK) {
		d->frame_irqs++;
		if (debug >= 3 || (debug >= 1 && d->frame_irqs <= 8))
			dev_info(&d->pdev->dev,
				 "video irq #%lu: status=%08x done=%08x/%08x/%08x\n",
				 d->frame_irqs, st, v0, v1, v2);
		if (st & BIT(0))
			hd60_deliver_frame(d, v0 & 0xf);
	}
	if (st & ST_AUDIO_BUFS)
		hd60_audio_irq(d, (st & ST_AUDIO_BUFS) >> 16);
	if ((st & ~(ST_VIDEO_MASK | ST_AUDIO_MASK | ST_CMD_DONE)) && debug >= 1)
		dev_info(&d->pdev->dev, "unhandled status bits %08x\n", st);
}

static irqreturn_t hd60_irq(int irq, void *data)
{
	struct hd60_dev *d = data;

	return hd60_ack_status(d) ? IRQ_WAKE_THREAD : IRQ_NONE;
}

static irqreturn_t hd60_irq_thread(int irq, void *data)
{
	hd60_process_events(data);
	return IRQ_HANDLED;
}

static int hd60_poll_fn(void *data)
{
	struct hd60_dev *d = data;

	while (!kthread_should_stop()) {
		if (hd60_ack_status(d))
			hd60_process_events(d);
		usleep_range(500, 1000);
	}
	return 0;
}

/* ======================================================================== */
/* command mailbox                                                          */
/* ======================================================================== */

static bool cmd_is_dangerous(u32 id)
{
	/* firmware / base firmware / logo download and flash commands */
	return (id >= 0x0b && id <= 0x0f) || id == 0x7b || id == 0x7c ||
	       id == 0x60 || id == 0x61;
}

/*
 * Post a command and wait for completion.  The card signals completion with
 * status bit 11 (via interrupt or seen by the poll thread); most commands
 * additionally write an acknowledge word to BAR0+0x2c.  Commands with ten
 * parameters overwrite the acknowledge slot with their last parameter, so for
 * those only the status bit (or a change of that word) counts.
 */
static int hd60_cmd(struct hd60_dev *d, u32 id, const u32 *p, int np,
		    u32 *ack, unsigned int timeout_ms)
{
	unsigned long end;
	u32 a = 0, idle_ack;
	int i, ret = -ETIMEDOUT;

	if (np > 10)
		return -EINVAL;

	mutex_lock(&d->cmd_lock);
	atomic_set(&d->cmd_done, 0);
	w0(d, R0_ACK, 0);
	w0(d, R0_CMD, id);
	for (i = 0; i < np; i++)
		w0(d, R0_PARAM(i), p[i]);
	idle_ack = np == 10 ? p[9] : 0;
	wmb();
	w0(d, R0_DOORBELL, DB_CMD);

	end = jiffies + msecs_to_jiffies(timeout_ms);
	do {
		wait_event_timeout(d->cmd_wq, atomic_read(&d->cmd_done),
				   msecs_to_jiffies(2));
		a = r0(d, R0_ACK);
		if (atomic_read(&d->cmd_done) ||
		    (a != idle_ack && a != 0 && a != 0xffffffff)) {
			if (atomic_read(&d->cmd_done))
				udelay(50);	/* let a late ack word land */
			a = r0(d, R0_ACK);
			ret = 0;
			break;
		}
	} while (time_before(jiffies, end));

	for (i = 0; i < ARRAY_SIZE(d->dbg_last_resp); i++)
		d->dbg_last_resp[i] = r0(d, R0_PARAM(i));
	mutex_unlock(&d->cmd_lock);

	if (debug >= 2 || (ret && debug >= 1))
		dev_info(&d->pdev->dev, "cmd 0x%02x [%*ph] -> %s ack=%08x\n",
			 id, np * 4, p, ret ? "TIMEOUT" : "ok", a);
	if (ack)
		*ack = a;
	return ret;
}

/* ---- I2C (through the card) ------------------------------------------- */
#define CMD_I2C_RD	0x1a
#define CMD_I2C_WR	0x1b
#define CMD_GPIO_SET	0x15
#define CMD_GPIO_DIR	0x17
#define MST_ADDR	0x9c	/* MStar MST3367 HDMI receiver (8-bit address) */

static int hd60_i2c_wr(struct hd60_dev *d, u8 addr, u8 reg, u8 val)
{
	u32 p[3] = { addr, reg, val };

	return hd60_cmd(d, CMD_I2C_WR, p, 3, NULL, 500);
}

static int hd60_i2c_rd(struct hd60_dev *d, u8 addr, u8 reg, u8 *val)
{
	u32 p[3] = { addr, reg, 0 };
	int ret = hd60_cmd(d, CMD_I2C_RD, p, 3, NULL, 500);

	*val = ret ? 0 : (r0(d, 0x10) & 0xff);
	return ret;
}

/* MST3367 register access with the same page caching as the Windows driver */
static void mst_page(struct hd60_dev *d, u8 page)
{
	if (d->mst_page != page) {
		d->mst_page = page;
		hd60_i2c_wr(d, MST_ADDR, 0x00, page);
	}
}

static void mst_wr(struct hd60_dev *d, u8 page, u8 reg, u8 val)
{
	mst_page(d, page);
	hd60_i2c_wr(d, MST_ADDR, reg, val);
}

static u8 mst_rd(struct hd60_dev *d, u8 page, u8 reg)
{
	u8 v;

	mst_page(d, page);
	hd60_i2c_rd(d, MST_ADDR, reg, &v);
	return v;
}

static void hd60_gpio_set(struct hd60_dev *d, int pin, int val)
{
	u32 p[2] = { BIT(pin), (val ? 1u : 0u) << pin };

	hd60_cmd(d, CMD_GPIO_SET, p, 2, NULL, 500);
}

static void hd60_gpio_dir(struct hd60_dev *d, int pin, int input)
{
	u32 p[2] = { BIT(pin), (input ? 1u : 0u) << pin };

	hd60_cmd(d, CMD_GPIO_DIR, p, 2, NULL, 500);
}

/* MST3367 initialisation, replayed from the Windows driver */
enum { OP_W, OP_M, OP_R, OP_GP, OP_SL, OP_IF0 };
struct mst_op { u8 op, a, b, c, e; u16 ms; };
#define MW(pg, r, v)		{ OP_W, pg, r, v, 0, 0 }
#define MM(pg, r, keep, set)	{ OP_M, pg, r, keep, set, 0 }
#define MR(pg, r)		{ OP_R, pg, r, 0, 0, 0 }
#define GP(pin, v)		{ OP_GP, pin, v, 0, 0, 0 }
#define SL(t)			{ OP_SL, 0, 0, 0, 0, t }
#define IF0(n)			{ OP_IF0, n, 0, 0, 0, 0 }

static const struct mst_op mst_init_ops[] = {
	GP(9, 1),
	SL(50),
	GP(9, 0),
	SL(50),
	GP(9, 1),
	SL(50),
	GP(8, 0),
	SL(50),
	MW(0x00, 0x13, 0x08),
	GP(1, 0),
	MM(0x00, 0xb7, 0xfd, 0x02),
	MW(0x00, 0x41, 0x6f),
	MW(0x00, 0xb8, 0x00),
	MW(0x01, 0x0f, 0x02),
	MW(0x01, 0x16, 0x30),
	MW(0x00, 0x64, 0x02),
	MW(0x00, 0x65, 0xff),
	MW(0x00, 0x66, 0x00),
	MW(0x00, 0x67, 0x02),
	MW(0x01, 0x17, 0x02),
	MW(0x01, 0x18, 0x02),
	MW(0x01, 0x19, 0x02),
	MW(0x01, 0x1a, 0x50),
	MM(0x01, 0x2a, 0xf8, 0x07),
	MW(0x02, 0x08, 0x03),
	MW(0x01, 0x25, 0x00),
	MW(0x01, 0x26, 0x00),
	SL(20),
	MR(0x01, 0x27),
	SL(20),
	MR(0x01, 0x27),
	SL(20),
	MR(0x01, 0x27),
	SL(20),
	MR(0x01, 0x27),
	SL(20),
	MR(0x01, 0x27),
	MW(0x01, 0x24, 0x40),
	MR(0x01, 0x24),
	IF0(7),	/* only if bit0 of the read above is set */
	MW(0x01, 0x25, 0x00),
	MW(0x01, 0x26, 0x00),
	MW(0x01, 0x27, 0x00),
	MW(0x01, 0x27, 0x00),
	MW(0x01, 0x27, 0x00),
	MW(0x01, 0x27, 0x00),
	MW(0x01, 0x27, 0x00),
	MW(0x01, 0x30, 0x80),
	MW(0x01, 0x31, 0x00),
	MW(0x01, 0x32, 0x00),
	MW(0x00, 0xb0, 0x14),
	MM(0x00, 0xae, 0xfb, 0x04),
	MW(0x00, 0xad, 0x05),
	MW(0x00, 0xb1, 0xc0),
	MW(0x00, 0xb2, 0x00),
	MW(0x00, 0xb3, 0x00),
	MW(0x00, 0xb4, 0x55),
	MM(0x00, 0xb4, 0xfc, 0x00),
	MW(0x02, 0x01, 0x61),
	MW(0x02, 0x02, 0xf5),
	MM(0x02, 0x03, 0xfd, 0x02),
	MW(0x02, 0x04, 0x01),
	MW(0x02, 0x05, 0x00),
	MW(0x02, 0x06, 0x08),
	MW(0x02, 0x1c, 0x1a),
	MW(0x02, 0x1d, 0x00),
	MW(0x02, 0x1e, 0x00),
	MW(0x02, 0x1f, 0x00),
	MM(0x02, 0x25, 0x5d, 0xa2),
	MM(0x02, 0x02, 0x7f, 0x80),
	MM(0x02, 0x07, 0xfb, 0x04),
	MW(0x02, 0x17, 0xc0),
	MW(0x02, 0x19, 0xff),
	MW(0x02, 0x1a, 0xff),
	MW(0x02, 0x1b, 0xfc),
	MW(0x02, 0x20, 0x00),
	MM(0x02, 0x21, 0xfc, 0x00),
	MW(0x02, 0x22, 0x26),
	MW(0x02, 0x27, 0x00),
	MM(0x02, 0x2e, 0x5e, 0xa1),
	MM(0x00, 0xab, 0x80, 0x15),
	MM(0x00, 0xac, 0xc0, 0x15),
	GP(1, 0),
	MM(0x00, 0xb7, 0xfd, 0x02),
	MW(0x00, 0xb8, 0x10),
	MW(0x00, 0xb8, 0x00),
	MW(0x02, 0x07, 0xf4),
	MW(0x02, 0x07, 0x04),
	MW(0x00, 0x51, 0x89),
	GP(1, 0),
	MM(0x00, 0xb7, 0xfd, 0x00),
	MW(0x00, 0xb7, 0x00),
	MM(0x02, 0x01, 0x0f, 0x60),
	MM(0x02, 0x04, 0xfe, 0x01),
	MW(0x02, 0x06, 0x08),
	MM(0x02, 0x09, 0xdf, 0x20),
	MM(0x00, 0x54, 0xef, 0x00),
	MM(0x00, 0xac, 0x7f, 0x80),
	MM(0x00, 0x00, 0x7f, 0x80),
	MM(0x00, 0xce, 0x7f, 0x80),
	MM(0x00, 0xcf, 0xf8, 0x02),
	MM(0x00, 0x00, 0x7f, 0x00),
	MM(0x80, 0xd0, 0xfc, 0x03),
	MM(0x80, 0xcf, 0x7f, 0x00),
	SL(300),
};

static void hd60_mst_init(struct hd60_dev *d)
{
	const struct mst_op *o;
	int i, skip = 0;
	u8 last = 0, v;

	d->mst_page = 0xff;
	for (i = 0; i < ARRAY_SIZE(mst_init_ops); i++) {
		o = &mst_init_ops[i];
		if (skip) {
			skip--;
			continue;
		}
		switch (o->op) {
		case OP_W:
			mst_wr(d, o->a, o->b, o->c);
			break;
		case OP_M:
			v = mst_rd(d, o->a, o->b);
			mst_wr(d, o->a, o->b, (v & o->c) | o->e);
			break;
		case OP_R:
			last = mst_rd(d, o->a, o->b);
			break;
		case OP_GP:
			hd60_gpio_set(d, o->a, o->b);
			break;
		case OP_SL:
			msleep(o->ms);
			break;
		case OP_IF0:
			if (!(last & 1))
				skip = o->a;
			break;
		}
	}
	dev_info(&d->pdev->dev, "MST3367 initialised\n");
}

/* standard modes keyed by total lines per frame (progressive) */
static const struct { u16 vtotal, height; } hd60_vmodes[] = {
	{ 1125, 1080 }, { 750, 720 }, { 525, 480 }, { 625, 576 }, { 806, 768 },
	{ 628, 600 }, { 1066, 1024 }, { 1235, 1200 }, { 1089, 1050 }, { 798, 768 },
	{ 790, 768 }, { 1000, 960 }, { 934, 900 }, { 449, 400 }, { 500, 480 },
	{ 666, 600 }, { 823, 800 },
};

/*
 * Colour-space conversion setup done by the Windows driver after a mode is
 * detected (default brightness/contrast/saturation/hue = 128).  RGB input is
 * converted to BT.709 YCbCr, YCbCr input passes through.
 */
static const u8 mst_csc_regs[24] = {
	0x9c, 0x9b, 0x96, 0x95, 0xa2, 0xa1, 0x9a, 0x99, 0x94, 0x93, 0xa0, 0x9f,
	0x9e, 0x9d, 0x98, 0x97, 0xa4, 0xa3, 0xa6, 0xa5, 0xa8, 0xa7, 0xaa, 0xa9,
};
static const u8 mst_csc_rgb[24] = {
	0x9f, 0x0b, 0x74, 0x03, 0x2c, 0x01, 0x94, 0x78, 0x2d, 0x08, 0x40, 0x7f,
	0xb3, 0x79, 0x22, 0x7e, 0x2d, 0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20,
};
static const u8 mst_csc_yuv[24] = {
	0x3f, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20,
};

static void hd60_mst_setmode(struct hd60_dev *d)
{
	u8 v, avi;
	bool yuv;
	int i;

	mst_wr(d, 0, 0xe2, 0x00);
	v = mst_rd(d, 2, 0x07);
	mst_wr(d, 2, 0x07, v | 0x10);
	mst_wr(d, 2, 0x07, v & ~0x10);
	avi = mst_rd(d, 2, 0x48);		/* AVI infoframe byte 1 */
	yuv = (avi & 0x60) != 0;

	v = mst_rd(d, 0, 0xab);
	mst_wr(d, 0, 0xab, (v & 0x7f) | 0x80);
	mst_wr(d, 0, 0x90, 0x15);
	mst_wr(d, 0, 0x91, 0x15);
	mst_wr(d, 0, 0x92, yuv ? 0x62 : 0x40);
	v = mst_rd(d, 0, 0xab);
	mst_wr(d, 0, 0xab, (v & 0x95) | 0x15);
	v = mst_rd(d, 0, 0xac);
	mst_wr(d, 0, 0xac, (v & 0xd5) | 0x15);
	mst_wr(d, 0, 0xad, 0x05);
	mst_wr(d, 0, 0x1e, 0x11);
	mst_wr(d, 0, 0x1f, 0x01);
	for (i = 0; i < ARRAY_SIZE(mst_csc_regs); i++)
		mst_wr(d, 0, mst_csc_regs[i], yuv ? mst_csc_yuv[i] : mst_csc_rgb[i]);
	v = mst_rd(d, 0, 0xb0);
	mst_wr(d, 0, 0xb0, (v & 0xc2) | 0x21);
	v = mst_rd(d, 0, 0xab);
	mst_wr(d, 0, 0xab, v & 0x7f);
	mst_wr(d, 2, 0x20, 0x00);
	dev_info(&d->pdev->dev, "MST3367: %s input (AVI %02x), colour conversion set\n",
		 yuv ? "YCbCr" : "RGB", avi);
}

/* Port of the Windows MST3367_HDMI_MODE_DETECT routine. */
static bool hd60_mst_detect(struct hd60_dev *d)
{
	u32 htotal, hp, vp, vtotal, hde, hfreq, vfreq, calc, i, h = 0;
	u8 st, f;
	bool il;

	mst_wr(d, 0, 0x00, 0x00);
	st = mst_rd(d, 0, 0x55);
	d->sig_raw[0] = st;
	if ((st & 0x3c) != 0x3c) {
		dbg(1, d, "MST3367: no stable signal (reg55=%02x)\n", st);
		return false;
	}
	htotal = ((mst_rd(d, 0, 0x6a) & 0x0f) << 8) | mst_rd(d, 0, 0x6b);
	hp = ((mst_rd(d, 0, 0x57) & 0x3f) << 8) | mst_rd(d, 0, 0x58);
	vp = ((mst_rd(d, 0, 0x59) & 0x3f) << 8) | mst_rd(d, 0, 0x5a);
	vtotal = ((mst_rd(d, 0, 0x5b) & 0x07) << 8) | mst_rd(d, 0, 0x5c);
	il = mst_rd(d, 0, 0x5f) & 0x08;
	hde = ((mst_rd(d, 2, 0x29) & 0x1f) << 8) | mst_rd(d, 2, 0x28);
	if (hde & 1)
		hde++;
	if (mst_rd(d, 1, 0x01) & 0x04) {	/* deep colour: TMDS clock is faster */
		f = mst_rd(d, 2, 0x47) & 0x0f;
		if (f == 5)
			htotal = htotal * 4 / 5;
		else if (f == 6)
			htotal = htotal * 2 / 3;
		else if (f == 7)
			htotal /= 2;
	}
	hfreq = hp ? 1600000 / hp : 0;		/* 100 Hz units */
	vfreq = vp ? 1250000 / vp : 0;		/* 0.1 Hz units */
	calc = vtotal ? hfreq * 1000 / vtotal : 0;
	dev_info(&d->pdev->dev,
		 "MST3367: reg55=%02x htotal=%u hde=%u vtotal=%u hfreq=%u.%ukHz vfreq=%u.%uHz (calc %u.%u) %s\n",
		 st, htotal, hde, vtotal, hfreq / 10, hfreq % 10, vfreq / 10, vfreq % 10,
		 calc / 10, calc % 10, il ? "interlaced" : "progressive");
	if (vtotal < 150 || vfreq < 200 || vfreq > 1500 || hde < 128 ||
	    (calc > vfreq ? calc - vfreq : vfreq - calc) > 5)
		return false;

	for (i = 0; i < ARRAY_SIZE(hd60_vmodes); i++) {
		u32 vt = il ? hd60_vmodes[i].vtotal / 2 : hd60_vmodes[i].vtotal;

		if (vtotal >= vt - 2 && vtotal <= vt + 2) {
			h = hd60_vmodes[i].height;
			break;
		}
	}
	if (!h)
		h = (vtotal * 24 / 25) & ~1;	/* rough guess for unknown modes */
	d->sig_w = hde;
	d->sig_h = h;
	d->sig_fps = (vfreq + 5) / 10;
	d->sig_interlaced = il;
	hd60_mst_setmode(d);
	return true;
}

static void hd60_detect_signal(struct hd60_dev *d, bool force)
{
	if (!force && d->sig_jiffies &&
	    time_before(jiffies, d->sig_jiffies + HZ / 2))
		return;
	d->sig_jiffies = jiffies;

	d->sig_valid = d->hw_ready && hd60_mst_detect(d);
	if (!d->sig_valid) {
		d->sig_w = 1920;
		d->sig_h = 1080;
		d->sig_fps = 60;
		d->sig_interlaced = false;
	}
	if (d->sig_fps < 10 || d->sig_fps > 120)
		d->sig_fps = 60;
	if (force_width > 0)
		d->sig_w = force_width;
	if (force_height > 0)
		d->sig_h = force_height;
	if (force_fps > 0)
		d->sig_fps = force_fps;
	dbg(1, d, "signal: %s %ux%u%s@%u\n", d->sig_valid ? "locked" : "NONE (defaults)",
	    d->sig_w, d->sig_h, d->sig_interlaced ? "i" : "p", d->sig_fps);
}

/* Choose output format for the current signal. */
static void hd60_pick_format(struct hd60_dev *d)
{
	u64 pixrate = (u64)d->sig_w * d->sig_h * d->sig_fps;
	int mode = pixmode;

	if (mode != 5 && mode != 6 && mode != 7) {
		/* same choice as the Windows driver: YV12 via tinyvenc7 or 5 */
		mode = (pixrate > 69120000ULL && pixrate <= 138240000ULL) ? 7 : 5;
	}

	d->cardmode = mode;
	d->width = d->sig_w;
	d->height = d->sig_h;
	d->fps = d->sig_fps;
	if (mode == 6) {
		d->pixfmt = V4L2_PIX_FMT_YUYV;
		d->bytesperline = d->width * 2;
		d->sizeimage = d->bytesperline * d->height;
	} else {
		d->pixfmt = V4L2_PIX_FMT_YUV420;
		d->bytesperline = d->width;
		d->sizeimage = d->width * d->height * 3 / 2;
	}
}

/* ======================================================================== */
/* hardware bring-up                                                        */
/* ======================================================================== */

static void hd60_free_dmabuf(struct hd60_dev *d, struct hd60_dmabuf *b)
{
	if (b->cpu)
		dma_free_coherent(&d->pdev->dev, b->size, b->cpu, b->dma);
	b->cpu = NULL;
}

static int hd60_alloc_dmabuf(struct hd60_dev *d, struct hd60_dmabuf *b, size_t size)
{
	b->size = size;
	b->cpu = dma_alloc_coherent(&d->pdev->dev, size, &b->dma, GFP_KERNEL);
	if (!b->cpu) {
		dev_err(&d->pdev->dev, "cannot allocate %zu byte DMA buffer\n", size);
		return -ENOMEM;
	}
	memset(b->cpu, 0, size);
	return 0;
}

static void hd60_free_buffers(struct hd60_dev *d)
{
	int i;

	for (i = 0; i < NUM_VBUF; i++) {
		hd60_free_dmabuf(d, &d->vbuf[i]);
		hd60_free_dmabuf(d, &d->ebuf_main[i]);
		hd60_free_dmabuf(d, &d->ebuf_sub[i]);
	}
	hd60_free_dmabuf(d, &d->abuf);
}

static int hd60_alloc_buffers(struct hd60_dev *d)
{
	int i, ret;

	for (i = 0; i < NUM_VBUF; i++) {
		ret = hd60_alloc_dmabuf(d, &d->vbuf[i], VBUF_SIZE);
		if (ret)
			return ret;
		if (reg_h264) {
			ret = hd60_alloc_dmabuf(d, &d->ebuf_main[i], EBUF_SIZE);
			if (!ret)
				ret = hd60_alloc_dmabuf(d, &d->ebuf_sub[i], EBUF_SIZE);
			if (ret)
				return ret;
		}
	}
	return hd60_alloc_dmabuf(d, &d->abuf,
				 ABUF_CHUNK * ABUF_PER_CH * ABUF_CHANNELS);
}

static void put_addr(u32 *p, dma_addr_t a)
{
	p[0] = upper_32_bits(a);
	p[1] = lower_32_bits(a);
}

static int hd60_register_buffers(struct hd60_dev *d)
{
	u32 p[10];
	int i, ch, ret;
	dma_addr_t a;

	/* preview (uncompressed) buffers, channel 0 */
	p[0] = 0;
	p[1] = VBUF_SIZE;
	for (i = 0; i < NUM_VBUF; i++)
		put_addr(&p[2 + 2 * i], d->vbuf[i].dma);
	ret = hd60_cmd(d, CMD_VBUF, p, 10, NULL, 5000);
	if (ret)
		return ret;

	/* audio buffers: 8 channels x 4 x 8 KiB, consecutive */
	a = d->abuf.dma;
	for (ch = 0; ch < ABUF_CHANNELS; ch++) {
		p[0] = ch;
		p[1] = ABUF_CHUNK;
		for (i = 0; i < ABUF_PER_CH; i++, a += ABUF_CHUNK)
			put_addr(&p[2 + 2 * i], a);
		ret = hd60_cmd(d, CMD_ABUF, p, 10, NULL, 5000);
		if (ret)
			return ret;
	}

	if (reg_h264) {
		p[0] = 0;
		p[1] = EBUF_SIZE;
		for (i = 0; i < NUM_VBUF; i++)
			put_addr(&p[2 + 2 * i], d->ebuf_main[i].dma);
		ret = hd60_cmd(d, CMD_EBUF_MAIN, p, 10, NULL, 5000);
		if (ret)
			return ret;
		for (i = 0; i < NUM_VBUF; i++)
			put_addr(&p[2 + 2 * i], d->ebuf_sub[i].dma);
		ret = hd60_cmd(d, CMD_EBUF_SUB, p, 10, NULL, 5000);
		if (ret)
			return ret;
	}
	return 0;
}

static void hd60_clear_irq(struct hd60_dev *d)
{
	w5(d, R5_INTX_CLR, 2);
	w0(d, R0_STATUS, 0);
	w0(d, R0_DOORBELL, DB_IRQ_ACK);
}

static int hd60_hw_init(struct hd60_dev *d)
{
	u32 ack, p[2] = { 0, 0 };
	int i, ret = -ETIMEDOUT;

	msleep(256);
	hd60_clear_irq(d);

	dev_info(&d->pdev->dev, "BAR0 bus address %pa; waiting for card firmware\n",
		 &d->bar0_bus);
	for (i = 0; i < 100; i++) {
		w5(d, R5_IB_START, lower_32_bits(d->bar0_bus) + 4);
		w5(d, R5_IB_LIMIT, lower_32_bits(d->bar0_bus) + 0x5f);
		ret = hd60_cmd(d, CMD_FW_READY, NULL, 0, &ack, 500);
		if (!ret)
			break;
	}
	if (ret) {
		dev_err(&d->pdev->dev, "card firmware did not answer (is the card powered/booted?)\n");
		return ret;
	}
	dev_info(&d->pdev->dev, "card firmware ready (ack %08x)\n", ack);
	hd60_clear_irq(d);

	for (i = 0; i < 100; i++) {
		ret = hd60_cmd(d, CMD_FW_VERSION, p, 2, &ack, 1000);
		if (!ret && ack == ACK_VER_OK)
			break;
		msleep(10);
	}
	msleep(100);
	d->fw_major = r0(d, R0_PARAM(0));
	d->fw_minor = r0(d, R0_PARAM(1));
	dev_info(&d->pdev->dev, "card firmware version %u.%02u (ack %08x)\n",
		 d->fw_major, d->fw_minor, ack);
	if (d->fw_major != 1 || d->fw_minor != 11)
		dev_warn(&d->pdev->dev,
			 "Windows driver 1.1.0.194 expects firmware 01.11; continuing anyway\n");

	ret = hd60_register_buffers(d);
	if (ret) {
		dev_err(&d->pdev->dev, "buffer registration failed\n");
		return ret;
	}

	/* GPIO directions as set by the Windows driver for this board */
	hd60_gpio_dir(d, 0, 1);
	hd60_gpio_dir(d, 1, 0);
	hd60_gpio_dir(d, 2, 0);
	hd60_gpio_dir(d, 6, 1);
	hd60_gpio_dir(d, 8, 0);
	hd60_gpio_dir(d, 9, 0);
	hd60_gpio_dir(d, 10, 0);
	hd60_gpio_dir(d, 11, 0);

	d->hw_ready = true;
	if (mst_init)
		hd60_mst_init(d);
	return 0;
}

static int hd60_hw_start(struct hd60_dev *d)
{
	u32 p[10], fps = d->fps & 0xff, ack;
	u32 w = d->width, h = d->height;
	u32 vi = d->sig_interlaced ? 7 : 6;	/* BT.1120 interlaced / progressive */
	int ret, i;

	/* Sequence captured from the Windows driver (1.1.0.194) for this board */
	for (i = 0x50; i <= 0x5c; i += 4)
		w0(d, i, 0);

	/* SET_VIC: video capture parameters for channel 0 */
	p[0] = (vi << 24) | (d->cardmode << 16) | (fps << 8) | 0;
	p[1] = (h << 16) | w;
	p[2] = 0;				/* m, flip, mirror */
	p[3] = 0x02010101;			/* colour info */
	p[4] = 0;				/* x_start, y_start */
	p[5] = (h << 16) | w;			/* input frame size */
	p[6] = ((d->sig_valid ? 0 : 1) << 24) | (0x0a << 16) | (clamp(bitstreams, 0, 2) & 0xff);
	p[7] = 0;				/* vanc, fast_kill, int_reduce, slave */
	p[8] = 0x80800000;			/* no-signal colour */
	ret = hd60_cmd(d, CMD_SET_VIC, p, 9, &ack, 5000);
	if (ret)
		return ret;

	/* SET_AIC: stereo 16 bit, 48 kHz, 256 frames x 4 periods, on, irq mode 1 */
	p[0] = (16 << 16) | (2 << 8) | 0;
	p[1] = 48000;
	p[2] = (4 << 16) | 256;
	p[3] = (1 << 8) | 1;
	if (hd60_cmd(d, CMD_SET_AIC, p, 4, &ack, 5000))
		dev_warn(&d->pdev->dev, "SET_AIC failed, continuing\n");

	/* H.264 encoder parameters (main and sub stream) */
	for (i = 0; i < 2; i++) {
		memset(p, 0, sizeof(p));
		p[0] = 0x3fff;
		p[1] = (fps << 16) | (i << 8);
		p[2] = 0x01000030;
		p[4] = 0x07000000;
		p[9] = 0x00050033;
		hd60_cmd(d, 0x2d, p, 10, NULL, 5000);
	}

	/* preview parameters: fps, deinterlace on */
	memset(p, 0, sizeof(p));
	p[0] = 0x1f;
	p[1] = fps << 8;
	p[2] = 1;
	hd60_cmd(d, 0x31, p, 5, NULL, 5000);

	/* clear OSD / overlay slots */
	for (i = 0; i < 0x18 * 2; i++) {
		memset(p, 0, sizeof(p));
		p[0] = ((i / 2) << 16) | ((i & 1) ? 0x80 : 0);
		hd60_cmd(d, 0x50, p, 10, NULL, 5000);
	}

	memset(p, 0, sizeof(p));
	p[0] = 0x00010000;
	hd60_cmd(d, 0x62, p, 2, NULL, 5000);

	for (i = 0; i < 2; i++) {
		memset(p, 0, sizeof(p));
		p[0] = i << 8;
		p[3] = 1;
		hd60_cmd(d, 0x51, p, 4, NULL, 5000);
	}

	p[0] = 0xffffffff;
	ret = hd60_cmd(d, CMD_START, p, 1, &ack, 5000);
	dev_info(&d->pdev->dev, "start %ux%u@%u mode %u: %s (ack %08x)\n",
		 w, h, fps, d->cardmode, ret ? "FAILED" : "ok", ack);
	return ret;
}

static void hd60_hw_stop(struct hd60_dev *d)
{
	u32 p[1] = { 0xffffffff };

	hd60_cmd(d, CMD_STOP, p, 1, NULL, 5000);
}

/* ======================================================================== */
/* frame delivery                                                           */
/* ======================================================================== */

static void hd60_copy_frame(struct hd60_dev *d, u8 *dst, const u8 *src)
{
	u32 w = d->width, h = d->height;

	if (d->cardmode == 6) {
		memcpy(dst, src, (size_t)w * h * 2);
	} else {
		u32 ys = ALIGN(w, 16), yh = ALIGN(h, max(y_align, 1));
		u32 cs = ys / 2, ch = yh / 2;
		const u8 *sy = src, *s1 = src + (size_t)ys * yh;
		const u8 *s2 = s1 + (size_t)cs * ch;
		const u8 *su = uv_swap ? s2 : s1, *sv = uv_swap ? s1 : s2;
		u8 *dy = dst, *du = dst + (size_t)w * h;
		u8 *dv = du + (size_t)(w / 2) * (h / 2);
		u32 y;

		/* card produces I420 (Y, U, V), verified on hardware */
		for (y = 0; y < h; y++)
			memcpy(dy + (size_t)y * w, sy + (size_t)y * ys, w);
		for (y = 0; y < h / 2; y++) {
			memcpy(du + (size_t)y * (w / 2), su + (size_t)y * cs, w / 2);
			memcpy(dv + (size_t)y * (w / 2), sv + (size_t)y * cs, w / 2);
		}
	}
}

static void hd60_deliver_frame(struct hd60_dev *d, int idx)
{
	struct hd60_buffer *buf;
	unsigned long flags;

	if (idx >= NUM_VBUF) {
		/* the nibble may be 1-based; accept 1..4 */
		if (idx >= 1 && idx <= NUM_VBUF)
			idx -= 1;
		else {
			dbg(1, d, "bad buffer index %d\n", idx);
			return;
		}
	}
	d->last_vidx = idx;
	if (!d->streaming)
		return;

	spin_lock_irqsave(&d->qlock, flags);
	buf = list_first_entry_or_null(&d->buf_list, struct hd60_buffer, list);
	if (buf)
		list_del(&buf->list);
	spin_unlock_irqrestore(&d->qlock, flags);

	if (!buf) {
		d->frames_dropped++;
		return;
	}
	dma_rmb();
	hd60_copy_frame(d, vb2_plane_vaddr(&buf->vb.vb2_buf, 0), d->vbuf[idx].cpu);
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, d->sizeimage);
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	if (!d->frames_out)
		d->first_frame = ktime_get();
	d->last_frame = ktime_get();
	buf->vb.sequence = d->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	d->frames_out++;
}

/* ======================================================================== */
/* videobuf2                                                                */
/* ======================================================================== */

static int hd60_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct hd60_dev *d = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < d->sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = d->sizeimage;
	if (*nbuffers < 3)
		*nbuffers = 3;
	return 0;
}

static int hd60_buf_prepare(struct vb2_buffer *vb)
{
	struct hd60_dev *d = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < d->sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, d->sizeimage);
	return 0;
}

static void hd60_buf_queue(struct vb2_buffer *vb)
{
	struct hd60_dev *d = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct hd60_buffer *buf = container_of(vbuf, struct hd60_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&d->qlock, flags);
	list_add_tail(&buf->list, &d->buf_list);
	spin_unlock_irqrestore(&d->qlock, flags);
}

static void hd60_return_all(struct hd60_dev *d, enum vb2_buffer_state state)
{
	struct hd60_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&d->qlock, flags);
	list_for_each_entry_safe(buf, tmp, &d->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&d->qlock, flags);
}


/* ======================================================================== */
/* hardware sharing between the video node and the ALSA device              */
/* ======================================================================== */

static int hd60_acquire(struct hd60_dev *d, unsigned int who)
{
	int ret = 0;

	mutex_lock(&d->hw_lock);
	if (!d->hw_users) {
		if (!vb2_is_streaming(&d->queue) && !(who & HW_VIDEO)) {
			hd60_detect_signal(d, true);
			hd60_pick_format(d);
		}
		ret = hd60_hw_start(d);
	}
	if (!ret)
		d->hw_users |= who;
	mutex_unlock(&d->hw_lock);
	return ret;
}

static void hd60_release(struct hd60_dev *d, unsigned int who)
{
	mutex_lock(&d->hw_lock);
	if (d->hw_users & who) {
		d->hw_users &= ~who;
		if (!d->hw_users)
			hd60_hw_stop(d);
	}
	mutex_unlock(&d->hw_lock);
}

/* ======================================================================== */
/* ALSA capture                                                             */
/* ======================================================================== */

static const struct snd_pcm_hardware hd60_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= 48000,
	.rate_max		= 48000,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= 256 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 64 * 1024,
	.periods_min		= 2,
	.periods_max		= 128,
};

static void hd60_audio_copy(struct hd60_dev *d, int idx)
{
	struct snd_pcm_substream *ss;
	struct snd_pcm_runtime *rt;
	size_t bytes, chunk, n, periodb;
	const u8 *src;
	bool elapsed = false;
	unsigned long flags;

	spin_lock_irqsave(&d->alock, flags);
	ss = d->asub;
	if (!ss || !d->a_running) {
		spin_unlock_irqrestore(&d->alock, flags);
		return;
	}
	rt = ss->runtime;
	bytes = frames_to_bytes(rt, rt->buffer_size);
	periodb = frames_to_bytes(rt, rt->period_size);
	chunk = clamp(audio_chunk, 4, ABUF_CHUNK);
	src = (const u8 *)d->abuf.cpu + idx * ABUF_CHUNK;
	d->a_copies++;
	while (chunk) {
		n = min(chunk, bytes - d->a_pos);
		memcpy(rt->dma_area + d->a_pos, src, n);
		src += n;
		chunk -= n;
		d->a_pos = (d->a_pos + n) % bytes;
		d->a_period_acc += n;
	}
	if (d->a_period_acc >= periodb) {
		d->a_period_acc %= periodb;
		elapsed = true;
	}
	spin_unlock_irqrestore(&d->alock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(ss);
}

static void hd60_audio_irq(struct hd60_dev *d, u32 mask)
{
	int i, n = hweight32(mask), idx;

	{
		unsigned int k = d->atrace_n++ % ARRAY_SIZE(d->atrace);

		d->atrace[k].t = ktime_get_ns();
		d->atrace[k].mask = mask;
		d->atrace[k].pos = d->a_pos / 4;
		d->atrace[k].st = d->a_copies;
	}
	d->audio_irqs++;
	if (n > 1)
		d->audio_multi++;
	if (debug >= 3 || (debug >= 1 && d->audio_irqs <= 8))
		dev_info(&d->pdev->dev, "audio irq #%lu: buffers %x\n", d->audio_irqs, mask);
	dma_rmb();
	/* deliver in ring order, starting after the last buffer we took */
	{
		int base = d->last_aidx;

		for (i = 1; i <= 4; i++) {
			idx = (base + i) & 3;
			if (mask & BIT(idx)) {
				hd60_audio_copy(d, idx);
				d->last_aidx = idx;
			}
		}
	}
}

static int hd60_pcm_open(struct snd_pcm_substream *ss)
{
	struct hd60_dev *d = snd_pcm_substream_chip(ss);

	ss->runtime->hw = hd60_pcm_hw;
	snd_pcm_hw_constraint_integer(ss->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	d->asub = ss;
	return 0;
}

static int hd60_pcm_close(struct snd_pcm_substream *ss)
{
	struct hd60_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&d->alock, flags);
	d->a_running = false;
	d->asub = NULL;
	spin_unlock_irqrestore(&d->alock, flags);
	hd60_release(d, HW_AUDIO);
	return 0;
}

static int hd60_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct hd60_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;

	if (!d->hw_ready)
		return -EIO;
	spin_lock_irqsave(&d->alock, flags);
	d->a_pos = 0;
	d->a_period_acc = 0;
	spin_unlock_irqrestore(&d->alock, flags);
	/* the card only produces audio while capture is running */
	return hd60_acquire(d, HW_AUDIO);
}

static int hd60_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct hd60_dev *d = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		d->a_running = true;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		d->a_running = false;
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t hd60_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct hd60_dev *d = snd_pcm_substream_chip(ss);

	return bytes_to_frames(ss->runtime, d->a_pos);
}

static const struct snd_pcm_ops hd60_pcm_ops = {
	.open		= hd60_pcm_open,
	.close		= hd60_pcm_close,
	.prepare	= hd60_pcm_prepare,
	.trigger	= hd60_pcm_trigger,
	.pointer	= hd60_pcm_pointer,
};

static int hd60_alsa_init(struct hd60_dev *d)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	ret = snd_card_new(&d->pdev->dev, SNDRV_DEFAULT_IDX1, "HD60Pro",
			   THIS_MODULE, 0, &card);
	if (ret)
		return ret;
	strscpy(card->driver, DRV_NAME, sizeof(card->driver));
	strscpy(card->shortname, "Elgato HD60 Pro", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "Elgato Game Capture HD60 Pro at %s",
		 pci_name(d->pdev));
	ret = snd_pcm_new(card, "HD60 Pro HDMI", 0, 0, 1, &pcm);
	if (ret)
		goto err;
	pcm->private_data = d;
	strscpy(pcm->name, "HD60 Pro HDMI", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &hd60_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	ret = snd_card_register(card);
	if (ret)
		goto err;
	d->card = card;
	d->pcm = pcm;
	dev_info(&d->pdev->dev, "audio: ALSA card %d (\"%s\")\n", card->number, card->shortname);
	return 0;
err:
	snd_card_free(card);
	return ret;
}

static int hd60_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hd60_dev *d = vb2_get_drv_priv(q);
	int ret;

	if (!d->hw_ready) {
		hd60_return_all(d, VB2_BUF_STATE_QUEUED);
		return -EIO;
	}
	d->sequence = 0;
	d->frames_out = 0;
	d->frame_irqs = 0;
	d->frames_dropped = 0;
	d->stream_start = ktime_get();
	d->streaming = true;
	ret = hd60_acquire(d, HW_VIDEO);
	if (ret) {
		d->streaming = false;
		hd60_return_all(d, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	return 0;
}

static void hd60_stop_streaming(struct vb2_queue *q)
{
	struct hd60_dev *d = vb2_get_drv_priv(q);

	d->streaming = false;
	hd60_release(d, HW_VIDEO);
	if (d->irq_ok)
		synchronize_irq(d->irq);
	hd60_return_all(d, VB2_BUF_STATE_ERROR);
	{
		s64 us = ktime_us_delta(d->last_frame, d->first_frame);
		s64 fps100 = (us > 0 && d->frames_out > 1) ?
			     div64_s64((s64)(d->frames_out - 1) * 100000000LL, us) : 0;

		dev_info(&d->pdev->dev, "stopped: %lu frames delivered, %lu dropped, %lld.%02lld fps\n",
			 d->frames_out, d->frames_dropped, fps100 / 100, fps100 % 100);
	}
}

static const struct vb2_ops hd60_vb2_ops = {
	.queue_setup		= hd60_queue_setup,
	.buf_prepare		= hd60_buf_prepare,
	.buf_queue		= hd60_buf_queue,
	.start_streaming	= hd60_start_streaming,
	.stop_streaming		= hd60_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
#endif
};

/* ======================================================================== */
/* V4L2 ioctls                                                              */
/* ======================================================================== */

static void hd60_refresh_format(struct hd60_dev *d)
{
	if (vb2_is_busy(&d->queue))
		return;
	hd60_detect_signal(d, false);
	hd60_pick_format(d);
}

static int hd60_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct hd60_dev *d = video_drvdata(file);

	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Elgato Game Capture HD60 Pro", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s", pci_name(d->pdev));
	return 0;
}

static int hd60_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct hd60_dev *d = video_drvdata(file);

	if (f->index)
		return -EINVAL;
	hd60_refresh_format(d);
	f->pixelformat = d->pixfmt;
	return 0;
}

static void hd60_fill_fmt(struct hd60_dev *d, struct v4l2_pix_format *pix)
{
	pix->width = d->width;
	pix->height = d->height;
	pix->pixelformat = d->pixfmt;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = d->bytesperline;
	pix->sizeimage = d->sizeimage;
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static int hd60_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct hd60_dev *d = video_drvdata(file);

	hd60_refresh_format(d);
	hd60_fill_fmt(d, &f->fmt.pix);
	return 0;
}

static int hd60_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct hd60_dev *d = video_drvdata(file);

	if (vb2_is_busy(&d->queue)) {
		hd60_fill_fmt(d, &f->fmt.pix);
		return -EBUSY;
	}
	hd60_refresh_format(d);
	/* the format follows the input signal; anything else is coerced */
	hd60_fill_fmt(d, &f->fmt.pix);
	return 0;
}

static int hd60_enum_input(struct file *file, void *priv, struct v4l2_input *i)
{
	if (i->index)
		return -EINVAL;
	i->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(i->name, "HDMI", sizeof(i->name));
	return 0;
}

static int hd60_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int hd60_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int hd60_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fs)
{
	struct hd60_dev *d = video_drvdata(file);

	if (fs->index)
		return -EINVAL;
	hd60_refresh_format(d);
	if (fs->pixel_format != d->pixfmt)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = d->width;
	fs->discrete.height = d->height;
	return 0;
}

static int hd60_enum_frameintervals(struct file *file, void *priv,
				    struct v4l2_frmivalenum *fi)
{
	struct hd60_dev *d = video_drvdata(file);

	if (fi->index || fi->pixel_format != d->pixfmt ||
	    fi->width != d->width || fi->height != d->height)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = d->fps;
	return 0;
}

static int hd60_g_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct hd60_dev *d = video_drvdata(file);

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.readbuffers = 3;
	sp->parm.capture.timeperframe.numerator = 1;
	sp->parm.capture.timeperframe.denominator = d->fps;
	return 0;
}

static int hd60_log_status(struct file *file, void *priv)
{
	struct hd60_dev *d = video_drvdata(file);

	dev_info(&d->pdev->dev, "fw %u.%02u, irqs %lu, frame irqs %lu, out %lu, dropped %lu\n",
		 d->fw_major, d->fw_minor, d->irq_count, d->frame_irqs,
		 d->frames_out, d->frames_dropped);
	return 0;
}

static const struct v4l2_ioctl_ops hd60_ioctl_ops = {
	.vidioc_querycap		= hd60_querycap,
	.vidioc_enum_fmt_vid_cap	= hd60_enum_fmt,
	.vidioc_g_fmt_vid_cap		= hd60_g_fmt,
	.vidioc_s_fmt_vid_cap		= hd60_s_fmt,
	.vidioc_try_fmt_vid_cap		= hd60_s_fmt,
	.vidioc_enum_input		= hd60_enum_input,
	.vidioc_g_input			= hd60_g_input,
	.vidioc_s_input			= hd60_s_input,
	.vidioc_enum_framesizes		= hd60_enum_framesizes,
	.vidioc_enum_frameintervals	= hd60_enum_frameintervals,
	.vidioc_g_parm			= hd60_g_parm,
	.vidioc_s_parm			= hd60_g_parm,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_log_status		= hd60_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations hd60_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

/* ======================================================================== */
/* debugfs: register dump, raw commands, raw frame dump                     */
/* ======================================================================== */

static int hd60_dbg_regs_show(struct seq_file *s, void *unused)
{
	struct hd60_dev *d = s->private;
	int i;

	seq_printf(s, "fw %u.%02u hw_ready=%d irq=%d(%s) irqs=%lu frame_irqs=%lu out=%lu dropped=%lu last_idx=%d\n",
		   d->fw_major, d->fw_minor, d->hw_ready, d->irq,
		   d->irq_ok ? "ok" : (poll ? "poll" : "none"), d->irq_count,
		   d->frame_irqs, d->frames_out, d->frames_dropped, d->last_vidx);
	seq_printf(s, "signal %s %ux%u%s@%u raw %*ph\n", d->sig_valid ? "locked" : "none",
		   d->sig_w, d->sig_h, d->sig_interlaced ? "i" : "p", d->sig_fps,
		   24, d->sig_raw);
	seq_printf(s, "format %ux%u mode %u fourcc %.4s size %u\n", d->width, d->height,
		   d->cardmode, (char *)&d->pixfmt, d->sizeimage);
	for (i = 0; i < 0x60; i += 16)
		seq_printf(s, "BAR0+%02x: %08x %08x %08x %08x\n", i, r0(d, i),
			   r0(d, i + 4), r0(d, i + 8), r0(d, i + 12));
	seq_puts(s, "last response:");
	for (i = 0; i < ARRAY_SIZE(d->dbg_last_resp); i++)
		seq_printf(s, " %08x", d->dbg_last_resp[i]);
	seq_puts(s, "\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60_dbg_regs);

static ssize_t hd60_dbg_cmd_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct hd60_dev *d = f->private_data;
	char kbuf[256], *s = kbuf, *tok;
	u32 v[11], ack;
	int n = 0, ret;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = 0;
	while ((tok = strsep(&s, " \t\n")) && n < 11) {
		if (!*tok)
			continue;
		if (kstrtou32(tok, 0, &v[n++]))
			return -EINVAL;
	}
	if (!n)
		return -EINVAL;
	if (cmd_is_dangerous(v[0])) {
		dev_warn(&d->pdev->dev, "refusing firmware/flash command 0x%x\n", v[0]);
		return -EPERM;
	}
	ret = hd60_cmd(d, v[0], &v[1], n - 1, &ack, 5000);
	d->dbg_last_ret = ret;
	d->dbg_last_ack = ack;
	if (debug >= 2)
		dev_info(&d->pdev->dev, "debugfs cmd 0x%x: %s ack=%08x\n", v[0],
			 ret ? "timeout" : "ok", ack);
	return len;
}

static ssize_t hd60_dbg_cmd_read(struct file *f, char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	struct hd60_dev *d = f->private_data;
	char kbuf[160];
	int n, i;

	n = scnprintf(kbuf, sizeof(kbuf), "%d %08x", d->dbg_last_ret, d->dbg_last_ack);
	for (i = 0; i < ARRAY_SIZE(d->dbg_last_resp); i++)
		n += scnprintf(kbuf + n, sizeof(kbuf) - n, " %08x", d->dbg_last_resp[i]);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n, "\n");
	return simple_read_from_buffer(ubuf, len, ppos, kbuf, n);
}

static const struct file_operations hd60_dbg_cmd_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= hd60_dbg_cmd_write,
	.read	= hd60_dbg_cmd_read,
	.llseek	= default_llseek,
};

static ssize_t hd60_dbg_frame_read(struct file *f, char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	struct hd60_dev *d = f->private_data;
	int idx = d->last_vidx;

	if (idx < 0 || idx >= NUM_VBUF)
		idx = 0;
	return simple_read_from_buffer(ubuf, len, ppos, d->vbuf[idx].cpu, VBUF_SIZE);
}

static const struct file_operations hd60_dbg_frame_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= hd60_dbg_frame_read,
	.llseek	= default_llseek,
};

static ssize_t hd60_dbg_abuf_read(struct file *f, char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct hd60_dev *d = f->private_data;

	return simple_read_from_buffer(ubuf, len, ppos, d->abuf.cpu, d->abuf.size);
}

static const struct file_operations hd60_dbg_abuf_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= hd60_dbg_abuf_read,
	.llseek	= default_llseek,
};

static int hd60_dbg_atrace_show(struct seq_file *sf, void *unused)
{
	struct hd60_dev *d = sf->private;
	unsigned int i, n = min_t(unsigned int, d->atrace_n, ARRAY_SIZE(d->atrace));
	unsigned int start = d->atrace_n - n;

	seq_printf(sf, "audio irqs %lu multi %lu copies %lu\n", d->audio_irqs, d->audio_multi, d->a_copies);
	for (i = 0; i < n; i++) {
		unsigned int k = (start + i) % ARRAY_SIZE(d->atrace);

		seq_printf(sf, "%llu %x %u %u\n", d->atrace[k].t / 1000, d->atrace[k].mask,
			   d->atrace[k].pos, d->atrace[k].st);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hd60_dbg_atrace);

static int hd60_dbg_signal_set(void *data, u64 val)
{
	struct hd60_dev *d = data;

	hd60_detect_signal(d, true);
	hd60_pick_format(d);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(hd60_dbg_signal_fops, NULL, hd60_dbg_signal_set, "%llu\n");

static void hd60_debugfs_init(struct hd60_dev *d)
{
	d->dbg_dir = debugfs_create_dir(pci_name(d->pdev), NULL);
	debugfs_create_file("regs", 0400, d->dbg_dir, d, &hd60_dbg_regs_fops);
	debugfs_create_file("cmd", 0600, d->dbg_dir, d, &hd60_dbg_cmd_fops);
	debugfs_create_file("frame", 0400, d->dbg_dir, d, &hd60_dbg_frame_fops);
	debugfs_create_file("abuf", 0400, d->dbg_dir, d, &hd60_dbg_abuf_fops);
	debugfs_create_file("atrace", 0400, d->dbg_dir, d, &hd60_dbg_atrace_fops);
	debugfs_create_file("detect", 0200, d->dbg_dir, d, &hd60_dbg_signal_fops);
}

/* ======================================================================== */
/* PCI probe / remove                                                       */
/* ======================================================================== */

static int hd60_setup_irq(struct hd60_dev *d)
{
	struct pci_dev *pdev = d->pdev;
	unsigned int types = use_msi ? PCI_IRQ_ALL_TYPES : PCI_IRQ_INTX;
	int ret;

	if (poll)
		goto use_poll;
	ret = pci_alloc_irq_vectors(pdev, 1, 1, types);
	if (ret < 0)
		goto use_poll;
	d->irq = pci_irq_vector(pdev, 0);
	ret = request_threaded_irq(d->irq, hd60_irq, hd60_irq_thread,
				   pdev->msi_enabled ? 0 : IRQF_SHARED, DRV_NAME, d);
	if (ret) {
		pci_free_irq_vectors(pdev);
		goto use_poll;
	}
	d->irq_ok = true;
	dev_info(&pdev->dev, "using %s interrupt %d\n",
		 pdev->msi_enabled ? "MSI" : "INTx", d->irq);
	return 0;

use_poll:
	poll = true;
	d->poll_thread = kthread_run(hd60_poll_fn, d, DRV_NAME "-poll");
	if (IS_ERR(d->poll_thread)) {
		ret = PTR_ERR(d->poll_thread);
		d->poll_thread = NULL;
		return ret;
	}
	dev_info(&pdev->dev, "using status polling\n");
	return 0;
}

static void hd60_teardown_irq(struct hd60_dev *d)
{
	if (d->poll_thread) {
		kthread_stop(d->poll_thread);
		d->poll_thread = NULL;
	}
	if (d->irq_ok) {
		free_irq(d->irq, d);
		pci_free_irq_vectors(d->pdev);
		d->irq_ok = false;
	}
}

static int hd60_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct hd60_dev *d;
	struct vb2_queue *q;
	int ret;

	dev_info(&pdev->dev, DRV_NAME " " DRV_VERSION " (experimental), subsystem %04x:%04x\n",
		 pdev->subsystem_vendor, pdev->subsystem_device);

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->pdev = pdev;
	d->mst_page = 0xff;
	d->last_vidx = -1;
	mutex_init(&d->cmd_lock);
	mutex_init(&d->lock);
	mutex_init(&d->hw_lock);
	spin_lock_init(&d->alock);
	d->last_aidx = 3;
	spin_lock_init(&d->reg_lock);
	spin_lock_init(&d->evt_lock);
	spin_lock_init(&d->qlock);
	init_waitqueue_head(&d->cmd_wq);
	INIT_LIST_HEAD(&d->buf_list);
	pci_set_drvdata(pdev, d);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "no usable 32-bit DMA\n");
		return ret;
	}

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pdev, 5) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "unexpected BAR layout\n");
		return -ENODEV;
	}
	ret = pcim_iomap_regions(pdev, BIT(0) | BIT(5), DRV_NAME);
	if (ret)
		return ret;
	d->bar0 = pcim_iomap_table(pdev)[0];
	d->bar5 = pcim_iomap_table(pdev)[5];
	d->bar0_bus = pci_bus_address(pdev, 0);

	ret = hd60_alloc_buffers(d);
	if (ret)
		goto err_free;

	ret = hd60_setup_irq(d);
	if (ret)
		goto err_free;

	hd60_debugfs_init(d);

	ret = hd60_hw_init(d);
	if (ret)
		dev_err(&pdev->dev, "hardware init failed (%d); debugfs left available for diagnosis\n", ret);

	hd60_detect_signal(d, true);
	hd60_pick_format(d);

	ret = v4l2_device_register(&pdev->dev, &d->v4l2_dev);
	if (ret)
		goto err_irq;

	q = &d->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = d;
	q->buf_struct_size = sizeof(struct hd60_buffer);
	q->ops = &hd60_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &d->lock;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
	q->min_queued_buffers = 2;
#else
	q->min_buffers_needed = 2;
#endif
	ret = vb2_queue_init(q);
	if (ret)
		goto err_v4l2;

	strscpy(d->vdev.name, "Elgato HD60 Pro", sizeof(d->vdev.name));
	d->vdev.release = video_device_release_empty;
	d->vdev.fops = &hd60_fops;
	d->vdev.ioctl_ops = &hd60_ioctl_ops;
	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.queue = q;
	d->vdev.lock = &d->lock;
	d->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			      V4L2_CAP_READWRITE;
	video_set_drvdata(&d->vdev, d);
	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;

	dev_info(&pdev->dev, "registered as %s\n", video_device_node_name(&d->vdev));
	if (hd60_alsa_init(d))
		dev_warn(&pdev->dev, "audio device not available\n");
	return 0;

err_v4l2:
	v4l2_device_unregister(&d->v4l2_dev);
err_irq:
	debugfs_remove_recursive(d->dbg_dir);
	hd60_teardown_irq(d);
err_free:
	hd60_free_buffers(d);
	return ret;
}

static void hd60_remove(struct pci_dev *pdev)
{
	struct hd60_dev *d = pci_get_drvdata(pdev);

	if (d->card) {
		/* sound servers keep the control device open; don't block on them */
		snd_card_disconnect(d->card);
		snd_card_free_when_closed(d->card);
	}
	video_unregister_device(&d->vdev);
	v4l2_device_unregister(&d->v4l2_dev);
	debugfs_remove_recursive(d->dbg_dir);
	if (d->hw_users)
		hd60_hw_stop(d);
	hd60_teardown_irq(d);
	hd60_free_buffers(d);
}

static const struct pci_device_id hd60_ids[] = {
	{ PCI_DEVICE(0x12ab, 0x0380) },
	{ PCI_DEVICE(0x12ab, 0x0381) },
	{ }
};
MODULE_DEVICE_TABLE(pci, hd60_ids);

static struct pci_driver hd60_driver = {
	.name		= DRV_NAME,
	.id_table	= hd60_ids,
	.probe		= hd60_probe,
	.remove		= hd60_remove,
};
module_pci_driver(hd60_driver);

MODULE_DESCRIPTION("Experimental driver for Elgato Game Capture HD60 Pro (YUAN MZ0380)");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
