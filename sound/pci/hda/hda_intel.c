/*
 *
 *  hda_intel.c - Implementation of primary alsa driver code base
 *                for Intel HD Audio.
 *
 *  Copyright(c) 2004 Intel Corporation. All rights reserved.
 *
 *  Copyright (c) 2004 Takashi Iwai <tiwai@suse.de>
 *                     PeiSen Hou <pshou@realtek.com.tw>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along with
 *  this program; if not, write to the Free Software Foundation, Inc., 59
 *  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *  CONTACTS:
 *
 *  Matt Jared		matt.jared@intel.com
 *  Andy Kopp		andy.kopp@intel.com
 *  Dan Kogan		dan.d.kogan@intel.com
 *
 *  CHANGES:
 *
 *  2004.12.01	Major rewrite by tiwai, merged the work of pshou
 *
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/clk.h>
#ifdef CONFIG_X86
/* for snoop control */
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#endif
#include <sound/core.h>
#include <sound/initval.h>
#include "hda_codec.h"

#ifdef CONFIG_SND_HDA_VPR
#include <linux/nvmap.h>
#endif
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
#include <mach/powergate.h>
#endif

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static char *model[SNDRV_CARDS];
static int position_fix[SNDRV_CARDS];
static int bdl_pos_adj[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = -1};
static int probe_mask[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] = -1};
static int probe_only[SNDRV_CARDS];
static bool single_cmd;
static int enable_msi = -1;
#ifdef CONFIG_SND_HDA_PATCH_LOADER
static char *patch[SNDRV_CARDS];
#endif
#ifdef CONFIG_SND_HDA_INPUT_BEEP
static int beep_mode[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS-1)] =
					CONFIG_SND_HDA_INPUT_BEEP_MODE};
#endif

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for Intel HD audio interface.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for Intel HD audio interface.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable Intel HD audio interface.");
module_param_array(model, charp, NULL, 0444);
MODULE_PARM_DESC(model, "Use the given board model.");
module_param_array(position_fix, int, NULL, 0444);
MODULE_PARM_DESC(position_fix, "DMA pointer read method."
		 "(0 = auto, 1 = LPIB, 2 = POSBUF, 3 = VIACOMBO, 4 = COMBO).");
module_param_array(bdl_pos_adj, int, NULL, 0644);
MODULE_PARM_DESC(bdl_pos_adj, "BDL position adjustment offset.");
module_param_array(probe_mask, int, NULL, 0444);
MODULE_PARM_DESC(probe_mask, "Bitmask to probe codecs (default = -1).");
module_param_array(probe_only, int, NULL, 0444);
MODULE_PARM_DESC(probe_only, "Only probing and no codec initialization.");
module_param(single_cmd, bool, 0444);
MODULE_PARM_DESC(single_cmd, "Use single command to communicate with codecs "
		 "(for debugging only).");
module_param(enable_msi, bint, 0444);
MODULE_PARM_DESC(enable_msi, "Enable Message Signaled Interrupt (MSI)");
#ifdef CONFIG_SND_HDA_PATCH_LOADER
module_param_array(patch, charp, NULL, 0444);
MODULE_PARM_DESC(patch, "Patch file for Intel HD audio interface.");
#endif
#ifdef CONFIG_SND_HDA_INPUT_BEEP
module_param_array(beep_mode, int, NULL, 0444);
MODULE_PARM_DESC(beep_mode, "Select HDA Beep registration mode "
			    "(0=off, 1=on, 2=mute switch on/off) (default=1).");
#endif

#ifdef CONFIG_SND_HDA_POWER_SAVE
static int power_save = CONFIG_SND_HDA_POWER_SAVE_DEFAULT;
module_param(power_save, int, 0644);
MODULE_PARM_DESC(power_save, "Automatic power-saving timeout "
		 "(in second, 0 = disable).");

/* reset the HD-audio controller in power save mode.
 * this may give more power-saving, but will take longer time to
 * wake up.
 */
static bool power_save_controller = 1;
module_param(power_save_controller, bool, 0644);
MODULE_PARM_DESC(power_save_controller, "Reset controller in power save mode.");
#endif

static int align_buffer_size = -1;
module_param(align_buffer_size, bint, 0644);
MODULE_PARM_DESC(align_buffer_size,
		"Force buffer and period sizes to be multiple of 128 bytes.");

#ifdef CONFIG_X86
static bool hda_snoop = true;
module_param_named(snoop, hda_snoop, bool, 0444);
MODULE_PARM_DESC(snoop, "Enable/disable snooping");
#define azx_snoop(chip)		(chip)->snoop
#else
#define hda_snoop		true
#define azx_snoop(chip)		true
#endif


MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{Intel, ICH6},"
			 "{Intel, ICH6M},"
			 "{Intel, ICH7},"
			 "{Intel, ESB2},"
			 "{Intel, ICH8},"
			 "{Intel, ICH9},"
			 "{Intel, ICH10},"
			 "{Intel, PCH},"
			 "{Intel, CPT},"
			 "{Intel, PPT},"
			 "{Intel, LPT},"
			 "{Intel, PBG},"
			 "{Intel, SCH},"
			 "{ATI, SB450},"
			 "{ATI, SB600},"
			 "{ATI, RS600},"
			 "{ATI, RS690},"
			 "{ATI, RS780},"
			 "{ATI, R600},"
			 "{ATI, RV630},"
			 "{ATI, RV610},"
			 "{ATI, RV670},"
			 "{ATI, RV635},"
			 "{ATI, RV620},"
			 "{ATI, RV770},"
			 "{VIA, VT8251},"
			 "{VIA, VT8237A},"
			 "{SiS, SIS966},"
			 "{ULI, M5461}}");
MODULE_DESCRIPTION("Intel HDA driver");

#ifdef CONFIG_SND_VERBOSE_PRINTK
#define SFX	/* nop */
#else
#define SFX	"hda-intel: "
#endif

/*
 * registers
 */
#define ICH6_REG_GCAP			0x00
#define   ICH6_GCAP_64OK	(1 << 0)   /* 64bit address support */
#define   ICH6_GCAP_NSDO	(3 << 1)   /* # of serial data out signals */
#define   ICH6_GCAP_BSS		(31 << 3)  /* # of bidirectional streams */
#define   ICH6_GCAP_ISS		(15 << 8)  /* # of input streams */
#define   ICH6_GCAP_OSS		(15 << 12) /* # of output streams */
#define ICH6_REG_VMIN			0x02
#define ICH6_REG_VMAJ			0x03
#define ICH6_REG_OUTPAY			0x04
#define ICH6_REG_INPAY			0x06
#define ICH6_REG_GCTL			0x08
#define   ICH6_GCTL_RESET	(1 << 0)   /* controller reset */
#define   ICH6_GCTL_FCNTRL	(1 << 1)   /* flush control */
#define   ICH6_GCTL_UNSOL	(1 << 8)   /* accept unsol. response enable */
#define ICH6_REG_WAKEEN			0x0c
#define ICH6_REG_STATESTS		0x0e
#define ICH6_REG_GSTS			0x10
#define   ICH6_GSTS_FSTS	(1 << 1)   /* flush status */
#define ICH6_REG_INTCTL			0x20
#define ICH6_REG_INTSTS			0x24
#define ICH6_REG_WALLCLK		0x30	/* 24Mhz source */
#define ICH6_REG_OLD_SSYNC		0x34	/* SSYNC for old ICH */
#define ICH6_REG_SSYNC			0x38
#define ICH6_REG_CORBLBASE		0x40
#define ICH6_REG_CORBUBASE		0x44
#define ICH6_REG_CORBWP			0x48
#define ICH6_REG_CORBRP			0x4a
#define   ICH6_CORBRP_RST	(1 << 15)  /* read pointer reset */
#define ICH6_REG_CORBCTL		0x4c
#define   ICH6_CORBCTL_RUN	(1 << 1)   /* enable DMA */
#define   ICH6_CORBCTL_CMEIE	(1 << 0)   /* enable memory error irq */
#define ICH6_REG_CORBSTS		0x4d
#define   ICH6_CORBSTS_CMEI	(1 << 0)   /* memory error indication */
#define ICH6_REG_CORBSIZE		0x4e

#define ICH6_REG_RIRBLBASE		0x50
#define ICH6_REG_RIRBUBASE		0x54
#define ICH6_REG_RIRBWP			0x58
#define   ICH6_RIRBWP_RST	(1 << 15)  /* write pointer reset */
#define ICH6_REG_RINTCNT		0x5a
#define ICH6_REG_RIRBCTL		0x5c
#define   ICH6_RBCTL_IRQ_EN	(1 << 0)   /* enable IRQ */
#define   ICH6_RBCTL_DMA_EN	(1 << 1)   /* enable DMA */
#define   ICH6_RBCTL_OVERRUN_EN	(1 << 2)   /* enable overrun irq */
#define ICH6_REG_RIRBSTS		0x5d
#define   ICH6_RBSTS_IRQ	(1 << 0)   /* response irq */
#define   ICH6_RBSTS_OVERRUN	(1 << 2)   /* overrun irq */
#define ICH6_REG_RIRBSIZE		0x5e

#define ICH6_REG_IC			0x60
#define ICH6_REG_IR			0x64
#define ICH6_REG_IRS			0x68
#define   ICH6_IRS_VALID	(1<<1)
#define   ICH6_IRS_BUSY		(1<<0)

#define ICH6_REG_DPLBASE		0x70
#define ICH6_REG_DPUBASE		0x74
#define   ICH6_DPLBASE_ENABLE	0x1	/* Enable position buffer */

/* SD offset: SDI0=0x80, SDI1=0xa0, ... SDO3=0x160 */
enum { SDI0, SDI1, SDI2, SDI3, SDO0, SDO1, SDO2, SDO3 };

/* stream register offsets from stream base */
#define ICH6_REG_SD_CTL			0x00
#define ICH6_REG_SD_STS			0x03
#define ICH6_REG_SD_LPIB		0x04
#define ICH6_REG_SD_CBL			0x08
#define ICH6_REG_SD_LVI			0x0c
#define ICH6_REG_SD_FIFOW		0x0e
#define ICH6_REG_SD_FIFOSIZE		0x10
#define ICH6_REG_SD_FORMAT		0x12
#define ICH6_REG_SD_BDLPL		0x18
#define ICH6_REG_SD_BDLPU		0x1c

/* PCI space */
#define ICH6_PCIREG_TCSEL	0x44

/*
 * other constants
 */

/* max number of SDs */
/* ICH, ATI and VIA have 4 playback and 4 capture */
#define ICH6_NUM_CAPTURE	4
#define ICH6_NUM_PLAYBACK	4

/* ULI has 6 playback and 5 capture */
#define ULI_NUM_CAPTURE		5
#define ULI_NUM_PLAYBACK	6

/* ATI HDMI has 1 playback and 0 capture */
#define ATIHDMI_NUM_CAPTURE	0
#define ATIHDMI_NUM_PLAYBACK	1

/* TERA has 4 playback and 3 capture */
#define TERA_NUM_CAPTURE	3
#define TERA_NUM_PLAYBACK	4

/* this number is statically defined for simplicity */
#define MAX_AZX_DEV		16

/* max number of fragments - we may use more if allocating more pages for BDL */
#define BDL_SIZE		4096
#define AZX_MAX_BDL_ENTRIES	(BDL_SIZE / 16)
#define AZX_MAX_FRAG		32
/* max buffer size - no h/w limit, you can increase as you like */
#define AZX_MAX_BUF_SIZE	(1024*1024*1024)

/* RIRB int mask: overrun[2], response[0] */
#define RIRB_INT_RESPONSE	0x01
#define RIRB_INT_OVERRUN	0x04
#define RIRB_INT_MASK		0x05

/* STATESTS int mask: S3,SD2,SD1,SD0 */
#define AZX_MAX_CODECS		8
#define AZX_DEFAULT_CODECS	4
#define STATESTS_INT_MASK	((1 << AZX_MAX_CODECS) - 1)

/* SD_CTL bits */
#define SD_CTL_STREAM_RESET	0x01	/* stream reset bit */
#define SD_CTL_DMA_START	0x02	/* stream DMA start bit */
#define SD_CTL_STRIPE		(3 << 16)	/* stripe control */
#define SD_CTL_TRAFFIC_PRIO	(1 << 18)	/* traffic priority */
#define SD_CTL_DIR		(1 << 19)	/* bi-directional stream */
#define SD_CTL_STREAM_TAG_MASK	(0xf << 20)
#define SD_CTL_STREAM_TAG_SHIFT	20

/* SD_CTL and SD_STS */
#define SD_INT_DESC_ERR		0x10	/* descriptor error interrupt */
#define SD_INT_FIFO_ERR		0x08	/* FIFO error interrupt */
#define SD_INT_COMPLETE		0x04	/* completion interrupt */
#define SD_INT_MASK		(SD_INT_DESC_ERR|SD_INT_FIFO_ERR|\
				 SD_INT_COMPLETE)

/* SD_STS */
#define SD_STS_FIFO_READY	0x20	/* FIFO ready */

/* INTCTL and INTSTS */
#define ICH6_INT_ALL_STREAM	0xff	   /* all stream interrupts */
#define ICH6_INT_CTRL_EN	0x40000000 /* controller interrupt enable bit */
#define ICH6_INT_GLOBAL_EN	0x80000000 /* global interrupt enable bit */

/* below are so far hardcoded - should read registers in future */
#define ICH6_MAX_CORB_ENTRIES	256
#define ICH6_MAX_RIRB_ENTRIES	256

/* position fix mode */
enum {
	POS_FIX_AUTO,
	POS_FIX_LPIB,
	POS_FIX_POSBUF,
	POS_FIX_VIACOMBO,
	POS_FIX_COMBO,
};

/* Defines for ATI HD Audio support in SB450 south bridge */
#define ATI_SB450_HDAUDIO_MISC_CNTR2_ADDR   0x42
#define ATI_SB450_HDAUDIO_ENABLE_SNOOP      0x02

/* Defines for Nvidia HDA support */
#define NVIDIA_HDA_TRANSREG_ADDR      0x4e
#define NVIDIA_HDA_ENABLE_COHBITS     0x0f
#define NVIDIA_HDA_ISTRM_COH          0x4d
#define NVIDIA_HDA_OSTRM_COH          0x4c
#define NVIDIA_HDA_ENABLE_COHBIT      0x01

#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
/* Defines for Nvidia Tegra HDA support */
#define NVIDIA_TEGRA_HDA_BAR0_OFFSET           0x8000

#define NVIDIA_TEGRA_HDA_CFG_CMD_OFFSET        0x1004
#define NVIDIA_TEGRA_HDA_CFG_BAR0_OFFSET       0x1010

#define NVIDIA_TEGRA_HDA_ENABLE_IO_SPACE       (1 << 0)
#define NVIDIA_TEGRA_HDA_ENABLE_MEM_SPACE      (1 << 1)
#define NVIDIA_TEGRA_HDA_ENABLE_BUS_MASTER     (1 << 2)
#define NVIDIA_TEGRA_HDA_ENABLE_SERR           (1 << 8)
#define NVIDIA_TEGRA_HDA_DISABLE_INTR          (1 << 10)
#define NVIDIA_TEGRA_HDA_BAR0_INIT_PROGRAM     0xFFFFFFFF
#define NVIDIA_TEGRA_HDA_BAR0_FINAL_PROGRAM    (1 << 14)

/* IPFS */
#define NVIDIA_TEGRA_HDA_IPFS_CONFIG           0x180
#define NVIDIA_TEGRA_HDA_IPFS_EN_FPCI          0x1

#define NVIDIA_TEGRA_HDA_IPFS_FPCI_BAR0        0x80
#define NVIDIA_TEGRA_HDA_FPCI_BAR0_START       0x40

#define NVIDIA_TEGRA_HDA_IPFS_INTR_MASK        0x188
#define NVIDIA_TEGRA_HDA_IPFS_EN_INTR          (1 << 16)
#endif /* CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA */

/* Defines for Intel SCH HDA snoop control */
#define INTEL_SCH_HDA_DEVC      0x78
#define INTEL_SCH_HDA_DEVC_NOSNOOP       (0x1<<11)

/* Define IN stream 0 FIFO size offset in VIA controller */
#define VIA_IN_STREAM0_FIFO_SIZE_OFFSET	0x90
/* Define VIA HD Audio Device ID*/
#define VIA_HDAC_DEVICE_ID		0x3288

/* HD Audio class code */
#define PCI_CLASS_MULTIMEDIA_HD_AUDIO	0x0403

/*
 */

struct azx_dev {
	struct snd_dma_buffer bdl; /* BDL buffer */
	u32 *posbuf;		/* position buffer pointer */

	unsigned int bufsize;	/* size of the play buffer in bytes */
	unsigned int period_bytes; /* size of the period in bytes */
	unsigned int frags;	/* number for period in the play buffer */
	unsigned int fifo_size;	/* FIFO size */
	unsigned long start_wallclk;	/* start + minimum wallclk */
	unsigned long period_wallclk;	/* wallclk for period */

	void __iomem *sd_addr;	/* stream descriptor pointer */

	u32 sd_int_sta_mask;	/* stream int status mask */

	/* pcm support */
	struct snd_pcm_substream *substream;	/* assigned substream,
						 * set in PCM open
						 */
	unsigned int format_val;	/* format value to be set in the
					 * controller and the codec
					 */
	unsigned char stream_tag;	/* assigned stream */
	unsigned char index;		/* stream index */
	int assigned_key;		/* last device# key assigned to */

	unsigned int opened :1;
	unsigned int running :1;
	unsigned int irq_pending :1;
	/*
	 * For VIA:
	 *  A flag to ensure DMA position is 0
	 *  when link position is not greater than FIFO size
	 */
	unsigned int insufficient :1;
	unsigned int wc_marked:1;
};

/* CORB/RIRB */
struct azx_rb {
	u32 *buf;		/* CORB/RIRB buffer
				 * Each CORB entry is 4byte, RIRB is 8byte
				 */
	dma_addr_t addr;	/* physical address of CORB/RIRB buffer */
	/* for RIRB */
	unsigned short rp, wp;	/* read/write pointers */
	int cmds[AZX_MAX_CODECS];	/* number of pending requests */
	u32 res[AZX_MAX_CODECS];	/* last read value */
};

struct azx_pcm {
	struct azx *chip;
	struct snd_pcm *pcm;
	struct hda_codec *codec;
	struct hda_pcm_stream *hinfo[2];
	struct list_head list;
};

struct azx {
	struct snd_card *card;
	struct pci_dev *pci;
	struct platform_device *pdev;
	struct device *dev;
	int irq_id;
	int dev_index;

	/* chip type specific */
	int driver_type;
	unsigned int driver_caps;
	int playback_streams;
	int playback_index_offset;
	int capture_streams;
	int capture_index_offset;
	int num_streams;

	/* pci resources */
	unsigned long addr;
	void __iomem *remap_addr;
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
	void __iomem *remap_config_addr;
#endif
	int irq;

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	/* platform driver clocks */
	struct clk **platform_clks;
	int platform_clk_count;
	int platform_clk_enable;
#endif

#ifdef CONFIG_SND_HDA_VPR
	struct nvmap_client *hda_vpr;
	struct nvmap_handle_ref *handle_ref;
	unsigned char *vaddr;
	phys_addr_t paddr;
#endif

	/* locks */
	spinlock_t reg_lock;
	struct mutex open_mutex;

	/* streams (x num_streams) */
	struct azx_dev *azx_dev;

	/* PCM */
	struct list_head pcm_list; /* azx_pcm list */

	/* HD codec */
	unsigned short codec_mask;
	int  codec_probe_mask; /* copied from probe_mask option */
	struct hda_bus *bus;
	unsigned int beep_mode;

	/* CORB/RIRB */
	struct azx_rb corb;
	struct azx_rb rirb;

	/* CORB/RIRB and position buffers */
	struct snd_dma_buffer rb;
	struct snd_dma_buffer posbuf;

	/* flags */
	int position_fix[2]; /* for both playback/capture streams */
	int poll_count;
	unsigned int running :1;
	unsigned int initialized :1;
	unsigned int single_cmd :1;
	unsigned int polling_mode :1;
	unsigned int msi :1;
	unsigned int irq_pending_warned :1;
	unsigned int probing :1; /* codec probing phase */
	unsigned int snoop:1;
	unsigned int align_buffer_size:1;

	/* for debugging */
	unsigned int last_cmd[AZX_MAX_CODECS];

	/* for pending irqs */
	struct work_struct irq_pending_work;

	/* reboot notifier (for mysterious hangup problem at power-down) */
	struct notifier_block reboot_notifier;
};

/* driver types */
enum {
	AZX_DRIVER_ICH,
	AZX_DRIVER_PCH,
	AZX_DRIVER_SCH,
	AZX_DRIVER_ATI,
	AZX_DRIVER_ATIHDMI,
	AZX_DRIVER_ATIHDMI_NS,
	AZX_DRIVER_VIA,
	AZX_DRIVER_SIS,
	AZX_DRIVER_ULI,
	AZX_DRIVER_NVIDIA,
	AZX_DRIVER_NVIDIA_TEGRA,
	AZX_DRIVER_TERA,
	AZX_DRIVER_CTX,
	AZX_DRIVER_GENERIC,
	AZX_NUM_DRIVERS, /* keep this as last entry */
};

/* driver quirks (capabilities) */
/* bits 0-7 are used for indicating driver type */
#define AZX_DCAPS_NO_TCSEL	(1 << 8)	/* No Intel TCSEL bit */
#define AZX_DCAPS_NO_MSI	(1 << 9)	/* No MSI support */
#define AZX_DCAPS_ATI_SNOOP	(1 << 10)	/* ATI snoop enable */
#define AZX_DCAPS_NVIDIA_SNOOP	(1 << 11)	/* Nvidia snoop enable */
#define AZX_DCAPS_SCH_SNOOP	(1 << 12)	/* SCH/PCH snoop enable */
#define AZX_DCAPS_RIRB_DELAY	(1 << 13)	/* Long delay in read loop */
#define AZX_DCAPS_RIRB_PRE_DELAY (1 << 14)	/* Put a delay before read */
#define AZX_DCAPS_CTX_WORKAROUND (1 << 15)	/* X-Fi workaround */
#define AZX_DCAPS_POSFIX_LPIB	(1 << 16)	/* Use LPIB as default */
#define AZX_DCAPS_POSFIX_VIA	(1 << 17)	/* Use VIACOMBO as default */
#define AZX_DCAPS_NO_64BIT	(1 << 18)	/* No 64bit address */
#define AZX_DCAPS_SYNC_WRITE	(1 << 19)	/* sync each cmd write */
#define AZX_DCAPS_OLD_SSYNC	(1 << 20)	/* Old SSYNC reg for ICH */
#define AZX_DCAPS_BUFSIZE	(1 << 21)	/* no buffer size alignment */
#define AZX_DCAPS_ALIGN_BUFSIZE	(1 << 22)	/* buffer size alignment */

/* quirks for ATI SB / AMD Hudson */
#define AZX_DCAPS_PRESET_ATI_SB \
	(AZX_DCAPS_ATI_SNOOP | AZX_DCAPS_NO_TCSEL | \
	 AZX_DCAPS_SYNC_WRITE | AZX_DCAPS_POSFIX_LPIB)

/* quirks for ATI/AMD HDMI */
#define AZX_DCAPS_PRESET_ATI_HDMI \
	(AZX_DCAPS_NO_TCSEL | AZX_DCAPS_SYNC_WRITE | AZX_DCAPS_POSFIX_LPIB)

/* quirks for Nvidia */
#define AZX_DCAPS_PRESET_NVIDIA \
	(AZX_DCAPS_NVIDIA_SNOOP | AZX_DCAPS_RIRB_DELAY | AZX_DCAPS_NO_MSI |\
	 AZX_DCAPS_ALIGN_BUFSIZE)

static char *driver_short_names[] __devinitdata = {
	[AZX_DRIVER_ICH] = "HDA Intel",
	[AZX_DRIVER_PCH] = "HDA Intel PCH",
	[AZX_DRIVER_SCH] = "HDA Intel MID",
	[AZX_DRIVER_ATI] = "HDA ATI SB",
	[AZX_DRIVER_ATIHDMI] = "HDA ATI HDMI",
	[AZX_DRIVER_ATIHDMI_NS] = "HDA ATI HDMI",
	[AZX_DRIVER_VIA] = "HDA VIA VT82xx",
	[AZX_DRIVER_SIS] = "HDA SIS966",
	[AZX_DRIVER_ULI] = "HDA ULI M5461",
	[AZX_DRIVER_NVIDIA] = "HDA NVidia",
	[AZX_DRIVER_NVIDIA_TEGRA] = "HDA NVIDIA Tegra",
	[AZX_DRIVER_TERA] = "HDA Teradici",
	[AZX_DRIVER_CTX] = "HDA Creative",
	[AZX_DRIVER_GENERIC] = "HD-Audio Generic",
};

/*
 * macros for easy use
 */
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
#define MASK_LONG_ALIGN		0x3UL
#define SHIFT_BYTE		3
#define SHIFT_BITS(reg)		((reg & MASK_LONG_ALIGN) << SHIFT_BYTE)
#define ADDR_ALIGN_L(base, reg)	(base + (reg & ~MASK_LONG_ALIGN))
#define MASK(bits)		(BIT(bits) - 1)
#define MASK_REG(reg, bits)	(MASK(bits) << SHIFT_BITS(reg))

#define tegra_write(base, reg, val, bits) \
	writel((readl(ADDR_ALIGN_L(base, reg)) & ~MASK_REG(reg, bits)) | \
	       ((val) << SHIFT_BITS(reg)), ADDR_ALIGN_L(base, reg))

#define tegra_read(base, reg, bits) \
	((readl(ADDR_ALIGN_L(base, reg)) >> SHIFT_BITS(reg)) & MASK(bits))

#define azx_writel(chip, reg, value) \
	writel(value, (chip)->remap_addr + ICH6_REG_##reg)
#define azx_readl(chip, reg) \
	readl((chip)->remap_addr + ICH6_REG_##reg)
#define azx_writew(chip, reg, value) \
	tegra_write((chip)->remap_addr, ICH6_REG_##reg, value, 16)
#define azx_readw(chip, reg) \
	tegra_read((chip)->remap_addr, ICH6_REG_##reg, 16)
#define azx_writeb(chip, reg, value) \
	tegra_write((chip)->remap_addr, ICH6_REG_##reg, value, 8)
#define azx_readb(chip, reg) \
	tegra_read((chip)->remap_addr, ICH6_REG_##reg, 8)

#define azx_sd_writel(dev, reg, value) \
	writel(value, (dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_readl(dev, reg) \
	readl((dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_writew(dev, reg, value) \
	tegra_write((dev)->sd_addr, ICH6_REG_##reg, value, 16)
#define azx_sd_readw(dev, reg) \
	tegra_read((dev)->sd_addr, ICH6_REG_##reg, 16)
#define azx_sd_writeb(dev, reg, value) \
	tegra_write((dev)->sd_addr, ICH6_REG_##reg, value, 8)
#define azx_sd_readb(dev, reg) \
	tegra_read((dev)->sd_addr, ICH6_REG_##reg, 8)

#else /* CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA */
#define azx_writel(chip,reg,value) \
	writel(value, (chip)->remap_addr + ICH6_REG_##reg)
#define azx_readl(chip,reg) \
	readl((chip)->remap_addr + ICH6_REG_##reg)
#define azx_writew(chip,reg,value) \
	writew(value, (chip)->remap_addr + ICH6_REG_##reg)
#define azx_readw(chip,reg) \
	readw((chip)->remap_addr + ICH6_REG_##reg)
#define azx_writeb(chip,reg,value) \
	writeb(value, (chip)->remap_addr + ICH6_REG_##reg)
#define azx_readb(chip,reg) \
	readb((chip)->remap_addr + ICH6_REG_##reg)

#define azx_sd_writel(dev,reg,value) \
	writel(value, (dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_readl(dev,reg) \
	readl((dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_writew(dev,reg,value) \
	writew(value, (dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_readw(dev,reg) \
	readw((dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_writeb(dev,reg,value) \
	writeb(value, (dev)->sd_addr + ICH6_REG_##reg)
#define azx_sd_readb(dev,reg) \
	readb((dev)->sd_addr + ICH6_REG_##reg)

#endif /* CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA */

/* for pcm support */
#define get_azx_dev(substream) (substream->runtime->private_data)

#ifdef CONFIG_X86
static void __mark_pages_wc(struct azx *chip, struct snd_dma_buffer *dmab, bool on)
{
	int pages;

	if (azx_snoop(chip))
		return;
	if (!dmab || !dmab->area || !dmab->bytes)
		return;

#ifdef CONFIG_SND_DMA_SGBUF
	if (dmab->dev.type == SNDRV_DMA_TYPE_DEV_SG) {
		struct snd_sg_buf *sgbuf = dmab->private_data;
		if (on)
			set_pages_array_wc(sgbuf->page_table, sgbuf->pages);
		else
			set_pages_array_wb(sgbuf->page_table, sgbuf->pages);
		return;
	}
#endif

	pages = (dmab->bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (on)
		set_memory_wc((unsigned long)dmab->area, pages);
	else
		set_memory_wb((unsigned long)dmab->area, pages);
}

static inline void mark_pages_wc(struct azx *chip, struct snd_dma_buffer *buf,
				 bool on)
{
	__mark_pages_wc(chip, buf, on);
}
static inline void mark_runtime_wc(struct azx *chip, struct azx_dev *azx_dev,
				   struct snd_pcm_substream *substream, bool on)
{
	if (azx_dev->wc_marked != on) {
		__mark_pages_wc(chip, substream->runtime->dma_buffer_p, on);
		azx_dev->wc_marked = on;
	}
}
#else
/* NOP for other archs */
static inline void mark_pages_wc(struct azx *chip, struct snd_dma_buffer *buf,
				 bool on)
{
}
static inline void mark_runtime_wc(struct azx *chip, struct azx_dev *azx_dev,
				   struct snd_pcm_substream *substream, bool on)
{
}
#endif

static int azx_acquire_irq(struct azx *chip, int do_disconnect);
static int azx_send_cmd(struct hda_bus *bus, unsigned int val);
/*
 * Interface for HD codec
 */

/*
 * CORB / RIRB interface
 */
static int azx_alloc_cmd_io(struct azx *chip)
{
	int err;

	/* single page (at least 4096 bytes) must suffice for both ringbuffes */
	err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV,
				  chip->dev,
				  PAGE_SIZE, &chip->rb);
	if (err < 0) {
		snd_printk(KERN_ERR SFX "cannot allocate CORB/RIRB\n");
		return err;
	}
	mark_pages_wc(chip, &chip->rb, true);
	return 0;
}

static void azx_init_cmd_io(struct azx *chip)
{
	spin_lock_irq(&chip->reg_lock);
	/* CORB set up */
	chip->corb.addr = chip->rb.addr;
	chip->corb.buf = (u32 *)chip->rb.area;
	azx_writel(chip, CORBLBASE, (u32)chip->corb.addr);
	azx_writel(chip, CORBUBASE, upper_32_bits(chip->corb.addr));

	/* set the corb size to 256 entries (ULI requires explicitly) */
	azx_writeb(chip, CORBSIZE, 0x02);
	/* set the corb write pointer to 0 */
	azx_writew(chip, CORBWP, 0);
	/* reset the corb hw read pointer */
	azx_writew(chip, CORBRP, ICH6_CORBRP_RST);
	/* enable corb dma */
	azx_writeb(chip, CORBCTL, ICH6_CORBCTL_RUN);

	/* RIRB set up */
	chip->rirb.addr = chip->rb.addr + 2048;
	chip->rirb.buf = (u32 *)(chip->rb.area + 2048);
	chip->rirb.wp = chip->rirb.rp = 0;
	memset(chip->rirb.cmds, 0, sizeof(chip->rirb.cmds));
	azx_writel(chip, RIRBLBASE, (u32)chip->rirb.addr);
	azx_writel(chip, RIRBUBASE, upper_32_bits(chip->rirb.addr));

	/* set the rirb size to 256 entries (ULI requires explicitly) */
	azx_writeb(chip, RIRBSIZE, 0x02);
	/* reset the rirb hw write pointer */
	azx_writew(chip, RIRBWP, ICH6_RIRBWP_RST);
	/* set N=1, get RIRB response interrupt for new entry */
	if (chip->driver_caps & AZX_DCAPS_CTX_WORKAROUND)
		azx_writew(chip, RINTCNT, 0xc0);
	else
		azx_writew(chip, RINTCNT, 1);
	/* enable rirb dma and response irq */
	azx_writeb(chip, RIRBCTL, ICH6_RBCTL_DMA_EN | ICH6_RBCTL_IRQ_EN);
	spin_unlock_irq(&chip->reg_lock);
}

static void azx_free_cmd_io(struct azx *chip)
{
	spin_lock_irq(&chip->reg_lock);
	/* disable ringbuffer DMAs */
	azx_writeb(chip, RIRBCTL, 0);
	azx_writeb(chip, CORBCTL, 0);
	spin_unlock_irq(&chip->reg_lock);
}

static unsigned int azx_command_addr(u32 cmd)
{
	unsigned int addr = cmd >> 28;

	if (addr >= AZX_MAX_CODECS) {
		snd_BUG();
		addr = 0;
	}

	return addr;
}

static unsigned int azx_response_addr(u32 res)
{
	unsigned int addr = res & 0xf;

	if (addr >= AZX_MAX_CODECS) {
		snd_BUG();
		addr = 0;
	}

	return addr;
}

/* send a command */
static int azx_corb_send_cmd(struct hda_bus *bus, u32 val)
{
	struct azx *chip = bus->private_data;
	unsigned int addr = azx_command_addr(val);
	unsigned int wp;

	spin_lock_irq(&chip->reg_lock);

	/* add command to corb */
	wp = azx_readb(chip, CORBWP);
	wp++;
	wp %= ICH6_MAX_CORB_ENTRIES;

	chip->rirb.cmds[addr]++;
	chip->corb.buf[wp] = cpu_to_le32(val);
	azx_writel(chip, CORBWP, wp);

	spin_unlock_irq(&chip->reg_lock);

	return 0;
}

#define ICH6_RIRB_EX_UNSOL_EV	(1<<4)

/* retrieve RIRB entry - called from interrupt handler */
static void azx_update_rirb(struct azx *chip)
{
	unsigned int rp, wp;
	unsigned int addr;
	u32 res, res_ex;

	wp = azx_readb(chip, RIRBWP);
	if (wp == chip->rirb.wp)
		return;
	chip->rirb.wp = wp;

	while (chip->rirb.rp != wp) {
		chip->rirb.rp++;
		chip->rirb.rp %= ICH6_MAX_RIRB_ENTRIES;

		rp = chip->rirb.rp << 1; /* an RIRB entry is 8-bytes */
		res_ex = le32_to_cpu(chip->rirb.buf[rp + 1]);
		res = le32_to_cpu(chip->rirb.buf[rp]);
		addr = azx_response_addr(res_ex);
		if (res_ex & ICH6_RIRB_EX_UNSOL_EV)
			snd_hda_queue_unsol_event(chip->bus, res, res_ex);
		else if (chip->rirb.cmds[addr]) {
			chip->rirb.res[addr] = res;
			smp_wmb();
			chip->rirb.cmds[addr]--;
		} else
			snd_printk(KERN_ERR SFX "spurious response %#x:%#x, "
				   "last cmd=%#08x\n",
				   res, res_ex,
				   chip->last_cmd[addr]);
	}
}

/* receive a response */
static unsigned int azx_rirb_get_response(struct hda_bus *bus,
					  unsigned int addr)
{
	struct azx *chip = bus->private_data;
	unsigned long timeout;
	unsigned long loopcounter;
	int do_poll = 0;

 again:
	timeout = jiffies + msecs_to_jiffies(1000);

	for (loopcounter = 0;; loopcounter++) {
		if (chip->polling_mode || do_poll) {
			spin_lock_irq(&chip->reg_lock);
			azx_update_rirb(chip);
			spin_unlock_irq(&chip->reg_lock);
		}
		if (!chip->rirb.cmds[addr]) {
			smp_rmb();
			bus->rirb_error = 0;

			if (!do_poll)
				chip->poll_count = 0;
			return chip->rirb.res[addr]; /* the last value */
		}
		if (time_after(jiffies, timeout))
			break;
		if (bus->needs_damn_long_delay || loopcounter > 3000)
			msleep(2); /* temporary workaround */
		else {
			udelay(10);
			cond_resched();
		}
	}

	if (!chip->polling_mode && chip->poll_count < 2) {
		snd_printdd(SFX "azx_get_response timeout, "
			   "polling the codec once: last cmd=0x%08x\n",
			   chip->last_cmd[addr]);
		do_poll = 1;
		chip->poll_count++;
		goto again;
	}


	if (!chip->polling_mode) {
		snd_printk(KERN_WARNING SFX "azx_get_response timeout, "
			   "switching to polling mode: last cmd=0x%08x\n",
			   chip->last_cmd[addr]);
		chip->polling_mode = 1;
		goto again;
	}

	if (chip->msi) {
		snd_printk(KERN_WARNING SFX "No response from codec, "
			   "disabling MSI: last cmd=0x%08x\n",
			   chip->last_cmd[addr]);
		free_irq(chip->irq, chip);
		chip->irq = -1;
		pci_disable_msi(chip->pci);
		chip->msi = 0;
		if (azx_acquire_irq(chip, 1) < 0) {
			bus->rirb_error = 1;
			return -1;
		}
		goto again;
	}

	if (chip->probing) {
		/* If this critical timeout happens during the codec probing
		 * phase, this is likely an access to a non-existing codec
		 * slot.  Better to return an error and reset the system.
		 */
		return -1;
	}

	/* a fatal communication error; need either to reset or to fallback
	 * to the single_cmd mode
	 */
	bus->rirb_error = 1;
	if (bus->allow_bus_reset && !bus->response_reset && !bus->in_reset) {
		bus->response_reset = 1;
		return -1; /* give a chance to retry */
	}

	snd_printk(KERN_ERR "hda_intel: azx_get_response timeout, "
		   "switching to single_cmd mode: last cmd=0x%08x\n",
		   chip->last_cmd[addr]);
	chip->single_cmd = 1;
	bus->response_reset = 0;
	/* release CORB/RIRB */
	azx_free_cmd_io(chip);
	/* disable unsolicited responses */
	azx_writel(chip, GCTL, azx_readl(chip, GCTL) & ~ICH6_GCTL_UNSOL);
	return -1;
}

/*
 * Use the single immediate command instead of CORB/RIRB for simplicity
 *
 * Note: according to Intel, this is not preferred use.  The command was
 *       intended for the BIOS only, and may get confused with unsolicited
 *       responses.  So, we shouldn't use it for normal operation from the
 *       driver.
 *       I left the codes, however, for debugging/testing purposes.
 */

/* receive a response */
static int azx_single_wait_for_response(struct azx *chip, unsigned int addr)
{
	int timeout = 50;

	while (timeout--) {
		/* check IRV busy bit */
		if (azx_readw(chip, IRS) & ICH6_IRS_VALID) {
			/* reuse rirb.res as the response return value */
			chip->rirb.res[addr] = azx_readl(chip, IR);
			return 0;
		}
		udelay(1);
	}
	if (printk_ratelimit())
		snd_printd(SFX "get_response timeout: IRS=0x%x\n",
			   azx_readw(chip, IRS));
	chip->rirb.res[addr] = -1;
	return -EIO;
}

/* send a command */
static int azx_single_send_cmd(struct hda_bus *bus, u32 val)
{
	struct azx *chip = bus->private_data;
	unsigned int addr = azx_command_addr(val);
	int timeout = 50;

	bus->rirb_error = 0;
	while (timeout--) {
		/* check ICB busy bit */
		if (!((azx_readw(chip, IRS) & ICH6_IRS_BUSY))) {
			/* Clear IRV valid bit */
			azx_writew(chip, IRS, azx_readw(chip, IRS) |
				   ICH6_IRS_VALID);
			azx_writel(chip, IC, val);
			azx_writew(chip, IRS, azx_readw(chip, IRS) |
				   ICH6_IRS_BUSY);
			return azx_single_wait_for_response(chip, addr);
		}
		udelay(1);
	}
	if (printk_ratelimit())
		snd_printd(SFX "send_cmd timeout: IRS=0x%x, val=0x%x\n",
			   azx_readw(chip, IRS), val);
	return -EIO;
}

/* receive a response */
static unsigned int azx_single_get_response(struct hda_bus *bus,
					    unsigned int addr)
{
	struct azx *chip = bus->private_data;
	return chip->rirb.res[addr];
}

/*
 * The below are the main callbacks from hda_codec.
 *
 * They are just the skeleton to call sub-callbacks according to the
 * current setting of chip->single_cmd.
 */

/* send a command */
static int azx_send_cmd(struct hda_bus *bus, unsigned int val)
{
	struct azx *chip = bus->private_data;

	chip->last_cmd[azx_command_addr(val)] = val;
	if (chip->single_cmd)
		return azx_single_send_cmd(bus, val);
	else
		return azx_corb_send_cmd(bus, val);
}

/* get a response */
static unsigned int azx_get_response(struct hda_bus *bus,
				     unsigned int addr)
{
	struct azx *chip = bus->private_data;
	if (chip->single_cmd)
		return azx_single_get_response(bus, addr);
	else
		return azx_rirb_get_response(bus, addr);
}

#ifdef CONFIG_SND_HDA_POWER_SAVE
static void azx_power_notify(struct hda_bus *bus);
#endif

/* reset codec link */
static int azx_reset(struct azx *chip, int full_reset)
{
	int count;

	if (!full_reset)
		goto __skip;

	/* clear STATESTS */
	azx_writeb(chip, STATESTS, STATESTS_INT_MASK);

	/* reset controller */
	azx_writel(chip, GCTL, azx_readl(chip, GCTL) & ~ICH6_GCTL_RESET);

	count = 50;
	while (azx_readb(chip, GCTL) && --count)
		mdelay(1);

	/* delay for >= 100us for codec PLL to settle per spec
	 * Rev 0.9 section 5.5.1
	 */
	mdelay(1);

	/* Bring controller out of reset */
	azx_writeb(chip, GCTL, azx_readb(chip, GCTL) | ICH6_GCTL_RESET);

	count = 50;
	while (!azx_readb(chip, GCTL) && --count)
		mdelay(1);

	/* Brent Chartrand said to wait >= 540us for codecs to initialize */
	mdelay(1);

      __skip:
	/* check to see if controller is ready */
	if (!azx_readb(chip, GCTL)) {
		snd_printd(SFX "azx_reset: controller not ready!\n");
		return -EBUSY;
	}

	/* Accept unsolicited responses */
	if (!chip->single_cmd)
		azx_writel(chip, GCTL, azx_readl(chip, GCTL) |
			   ICH6_GCTL_UNSOL);

	/* detect codecs */
	if (!chip->codec_mask) {
		chip->codec_mask = azx_readw(chip, STATESTS);
		snd_printdd(SFX "codec_mask = 0x%x\n", chip->codec_mask);
	}

	return 0;
}


/*
 * Lowlevel interface
 */

/* enable interrupts */
static void azx_int_enable(struct azx *chip)
{
	/* enable controller CIE and GIE */
	azx_writel(chip, INTCTL, azx_readl(chip, INTCTL) |
		   ICH6_INT_CTRL_EN | ICH6_INT_GLOBAL_EN);
}

/* disable interrupts */
static void azx_int_disable(struct azx *chip)
{
	int i;

	/* disable interrupts in stream descriptor */
	for (i = 0; i < chip->num_streams; i++) {
		struct azx_dev *azx_dev = &chip->azx_dev[i];
		azx_sd_writeb(azx_dev, SD_CTL,
			      azx_sd_readb(azx_dev, SD_CTL) & ~SD_INT_MASK);
	}

	/* disable SIE for all streams */
	azx_writeb(chip, INTCTL, 0);

	/* disable controller CIE and GIE */
	azx_writel(chip, INTCTL, azx_readl(chip, INTCTL) &
		   ~(ICH6_INT_CTRL_EN | ICH6_INT_GLOBAL_EN));
}

/* clear interrupts */
static void azx_int_clear(struct azx *chip)
{
	int i;

	/* clear stream status */
	for (i = 0; i < chip->num_streams; i++) {
		struct azx_dev *azx_dev = &chip->azx_dev[i];
		azx_sd_writeb(azx_dev, SD_STS, SD_INT_MASK);
	}

	/* clear STATESTS */
	azx_writeb(chip, STATESTS, STATESTS_INT_MASK);

	/* clear rirb status */
	azx_writeb(chip, RIRBSTS, RIRB_INT_MASK);

	/* clear int status */
	azx_writel(chip, INTSTS, ICH6_INT_CTRL_EN | ICH6_INT_ALL_STREAM);
}

/* start a stream */
static void azx_stream_start(struct azx *chip, struct azx_dev *azx_dev)
{
	/*
	 * Before stream start, initialize parameter
	 */
	azx_dev->insufficient = 1;

	/* enable SIE */
	azx_writel(chip, INTCTL,
		   azx_readl(chip, INTCTL) | (1 << azx_dev->index));
	/* set DMA start and interrupt mask */
	azx_sd_writeb(azx_dev, SD_CTL, azx_sd_readb(azx_dev, SD_CTL) |
		      SD_CTL_DMA_START | SD_INT_MASK);
}

/* stop DMA */
static void azx_stream_clear(struct azx *chip, struct azx_dev *azx_dev)
{
	azx_sd_writeb(azx_dev, SD_CTL, azx_sd_readb(azx_dev, SD_CTL) &
		      ~(SD_CTL_DMA_START | SD_INT_MASK));
	azx_sd_writeb(azx_dev, SD_STS, SD_INT_MASK); /* to be sure */
}

/* stop a stream */
static void azx_stream_stop(struct azx *chip, struct azx_dev *azx_dev)
{
	azx_stream_clear(chip, azx_dev);
	/* disable SIE */
	azx_writel(chip, INTCTL,
		   azx_readl(chip, INTCTL) & ~(1 << azx_dev->index));
}


/*
 * reset and start the controller registers
 */
static void azx_init_chip(struct azx *chip, int full_reset)
{
	if (chip->initialized)
		return;

	/* reset controller */
	azx_reset(chip, full_reset);

	/* initialize interrupts */
	azx_int_clear(chip);
	azx_int_enable(chip);

	/* initialize the codec command I/O */
	if (!chip->single_cmd)
		azx_init_cmd_io(chip);

	/* program the position buffer */
	azx_writel(chip, DPLBASE, (u32)chip->posbuf.addr);
	azx_writel(chip, DPUBASE, upper_32_bits(chip->posbuf.addr));

	chip->initialized = 1;
}

/*
 * initialize the PCI registers
 */
/* update bits in a PCI register byte */
static void update_pci_byte(struct pci_dev *pci, unsigned int reg,
			    unsigned char mask, unsigned char val)
{
	unsigned char data;

	pci_read_config_byte(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_byte(pci, reg, data);
}

static void azx_init_pci(struct azx *chip)
{
	/* Clear bits 0-2 of PCI register TCSEL (at offset 0x44)
	 * TCSEL == Traffic Class Select Register, which sets PCI express QOS
	 * Ensuring these bits are 0 clears playback static on some HD Audio
	 * codecs.
	 * The PCI register TCSEL is defined in the Intel manuals.
	 */
	if (!(chip->driver_caps & AZX_DCAPS_NO_TCSEL)) {
		snd_printdd(SFX "Clearing TCSEL\n");
		update_pci_byte(chip->pci, ICH6_PCIREG_TCSEL, 0x07, 0);
	}

	/* For ATI SB450/600/700/800/900 and AMD Hudson azalia HD audio,
	 * we need to enable snoop.
	 */
	if (chip->driver_caps & AZX_DCAPS_ATI_SNOOP) {
		snd_printdd(SFX "Setting ATI snoop: %d\n", azx_snoop(chip));
		update_pci_byte(chip->pci,
				ATI_SB450_HDAUDIO_MISC_CNTR2_ADDR, 0x07,
				azx_snoop(chip) ? ATI_SB450_HDAUDIO_ENABLE_SNOOP : 0);
	}

	/* For NVIDIA HDA, enable snoop */
	if (chip->driver_caps & AZX_DCAPS_NVIDIA_SNOOP) {
		snd_printdd(SFX "Setting Nvidia snoop: %d\n", azx_snoop(chip));
		update_pci_byte(chip->pci,
				NVIDIA_HDA_TRANSREG_ADDR,
				0x0f, NVIDIA_HDA_ENABLE_COHBITS);
		update_pci_byte(chip->pci,
				NVIDIA_HDA_ISTRM_COH,
				0x01, NVIDIA_HDA_ENABLE_COHBIT);
		update_pci_byte(chip->pci,
				NVIDIA_HDA_OSTRM_COH,
				0x01, NVIDIA_HDA_ENABLE_COHBIT);
	}

	/* Enable SCH/PCH snoop if needed */
	if (chip->driver_caps & AZX_DCAPS_SCH_SNOOP) {
		unsigned short snoop;
		pci_read_config_word(chip->pci, INTEL_SCH_HDA_DEVC, &snoop);
		if ((!azx_snoop(chip) && !(snoop & INTEL_SCH_HDA_DEVC_NOSNOOP)) ||
		    (azx_snoop(chip) && (snoop & INTEL_SCH_HDA_DEVC_NOSNOOP))) {
			snoop &= ~INTEL_SCH_HDA_DEVC_NOSNOOP;
			if (!azx_snoop(chip))
				snoop |= INTEL_SCH_HDA_DEVC_NOSNOOP;
			pci_write_config_word(chip->pci, INTEL_SCH_HDA_DEVC, snoop);
			pci_read_config_word(chip->pci,
				INTEL_SCH_HDA_DEVC, &snoop);
		}
		snd_printdd(SFX "SCH snoop: %s\n",
				(snoop & INTEL_SCH_HDA_DEVC_NOSNOOP)
				? "Disabled" : "Enabled");
        }
}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
/*
 * initialize the platform specific registers
 */
static void reg_update_bits(void __iomem *base, unsigned int reg,
			    unsigned int mask, unsigned int val)
{
	unsigned int data;

	data = readl(base + reg);
	data &= ~mask;
	data |= (val & mask);
	writel(data, base + reg);
}

static void azx_init_platform(struct azx *chip)
{
	switch (chip->driver_type) {
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
	case AZX_DRIVER_NVIDIA_TEGRA:
		/*Enable the PCI access */
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_IPFS_CONFIG,
				NVIDIA_TEGRA_HDA_IPFS_EN_FPCI,
				NVIDIA_TEGRA_HDA_IPFS_EN_FPCI);
		/* Enable MEM/IO space and bus master */
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_CFG_CMD_OFFSET, 0x507,
				NVIDIA_TEGRA_HDA_ENABLE_MEM_SPACE |
				NVIDIA_TEGRA_HDA_ENABLE_IO_SPACE |
				NVIDIA_TEGRA_HDA_ENABLE_BUS_MASTER |
				NVIDIA_TEGRA_HDA_ENABLE_SERR);
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_CFG_BAR0_OFFSET, 0xFFFFFFFF,
				NVIDIA_TEGRA_HDA_BAR0_INIT_PROGRAM);
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_CFG_BAR0_OFFSET, 0xFFFFFFFF,
				NVIDIA_TEGRA_HDA_BAR0_FINAL_PROGRAM);
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_IPFS_FPCI_BAR0, 0xFFFFFFFF,
				NVIDIA_TEGRA_HDA_FPCI_BAR0_START);
		reg_update_bits(chip->remap_config_addr,
				NVIDIA_TEGRA_HDA_IPFS_INTR_MASK,
				NVIDIA_TEGRA_HDA_IPFS_EN_INTR,
				NVIDIA_TEGRA_HDA_IPFS_EN_INTR);
		break;
#endif
	default:
		break;
	}

	return;
}

static void azx_platform_enable_clocks(struct azx *chip)
{
	int i;

#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
#if !defined(CONFIG_ARCH_TEGRA_2x_SOC) && !defined(CONFIG_ARCH_TEGRA_3x_SOC)
	tegra_unpowergate_partition(TEGRA_POWERGATE_DISB);
#endif
#endif

	for (i = 0; i < chip->platform_clk_count; i++)
		clk_enable(chip->platform_clks[i]);

	chip->platform_clk_enable++;

}

static void azx_platform_disable_clocks(struct azx *chip)
{
	int i;

	if (!chip->platform_clk_enable)
		return;

	for (i = 0; i < chip->platform_clk_count; i++)
		clk_disable(chip->platform_clks[i]);

#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
#if !defined(CONFIG_ARCH_TEGRA_2x_SOC) && !defined(CONFIG_ARCH_TEGRA_3x_SOC)
	tegra_powergate_partition(TEGRA_POWERGATE_DISB);
#endif
#endif

	chip->platform_clk_enable--;
}
#endif /* CONFIG_SND_HDA_PLATFORM_DRIVER */

static int azx_position_ok(struct azx *chip, struct azx_dev *azx_dev);

/*
 * interrupt handler
 */
static irqreturn_t azx_interrupt(int irq, void *dev_id)
{
	struct azx *chip = dev_id;
	struct azx_dev *azx_dev;
	u32 status;
	u8 sd_status;
	int i, ok;

	spin_lock(&chip->reg_lock);

	status = azx_readl(chip, INTSTS);
	if (status == 0) {
		spin_unlock(&chip->reg_lock);
		return IRQ_NONE;
	}

	for (i = 0; i < chip->num_streams; i++) {
		azx_dev = &chip->azx_dev[i];
		if (status & azx_dev->sd_int_sta_mask) {
			sd_status = azx_sd_readb(azx_dev, SD_STS);
			azx_sd_writeb(azx_dev, SD_STS, SD_INT_MASK);
			if (!azx_dev->substream || !azx_dev->running ||
			    !(sd_status & SD_INT_COMPLETE))
				continue;
			/* check whether this IRQ is really acceptable */
			ok = azx_position_ok(chip, azx_dev);
			if (ok == 1) {
				azx_dev->irq_pending = 0;
				spin_unlock(&chip->reg_lock);
				snd_pcm_period_elapsed(azx_dev->substream);
				spin_lock(&chip->reg_lock);
			} else if (ok == 0 && chip->bus && chip->bus->workq) {
				/* bogus IRQ, process it later */
				azx_dev->irq_pending = 1;
				queue_work(chip->bus->workq,
					   &chip->irq_pending_work);
			}
		}
	}

	/* clear rirb int */
	status = azx_readb(chip, RIRBSTS);
	if (status & RIRB_INT_MASK) {
		if (status & RIRB_INT_RESPONSE) {
			if (chip->driver_caps & AZX_DCAPS_RIRB_PRE_DELAY)
				udelay(80);
			azx_update_rirb(chip);
		}
		azx_writeb(chip, RIRBSTS, RIRB_INT_MASK);
	}

#if 0
	/* clear state status int */
	if (azx_readb(chip, STATESTS) & 0x04)
		azx_writeb(chip, STATESTS, 0x04);
#endif
	spin_unlock(&chip->reg_lock);

	return IRQ_HANDLED;
}


/*
 * set up a BDL entry
 */
static int setup_bdle(struct snd_pcm_substream *substream,
		      struct azx_dev *azx_dev, u32 **bdlp,
		      int ofs, int size, int with_ioc)
{
	u32 *bdl = *bdlp;

	while (size > 0) {
		dma_addr_t addr;
		int chunk;

		if (azx_dev->frags >= AZX_MAX_BDL_ENTRIES)
			return -EINVAL;

		addr = snd_pcm_sgbuf_get_addr(substream, ofs);
		/* program the address field of the BDL entry */
		bdl[0] = cpu_to_le32((u32)addr);
		bdl[1] = cpu_to_le32(upper_32_bits(addr));
		/* program the size field of the BDL entry */
		chunk = snd_pcm_sgbuf_get_chunk_size(substream, ofs, size);
		bdl[2] = cpu_to_le32(chunk);
		/* program the IOC to enable interrupt
		 * only when the whole fragment is processed
		 */
		size -= chunk;
		bdl[3] = (size || !with_ioc) ? 0 : cpu_to_le32(0x01);
		bdl += 4;
		azx_dev->frags++;
		ofs += chunk;
	}
	*bdlp = bdl;
	return ofs;
}

/*
 * set up BDL entries
 */
static int azx_setup_periods(struct azx *chip,
			     struct snd_pcm_substream *substream,
			     struct azx_dev *azx_dev)
{
	u32 *bdl;
	int i, ofs, periods, period_bytes;
	int pos_adj;

	/* reset BDL address */
	azx_sd_writel(azx_dev, SD_BDLPL, 0);
	azx_sd_writel(azx_dev, SD_BDLPU, 0);

	period_bytes = azx_dev->period_bytes;
	periods = azx_dev->bufsize / period_bytes;

	/* program the initial BDL entries */
	bdl = (u32 *)azx_dev->bdl.area;
	ofs = 0;
	azx_dev->frags = 0;
	pos_adj = bdl_pos_adj[chip->dev_index];
	if (pos_adj > 0) {
		struct snd_pcm_runtime *runtime = substream->runtime;
		int pos_align = pos_adj;
		pos_adj = (pos_adj * runtime->rate + 47999) / 48000;
		if (!pos_adj)
			pos_adj = pos_align;
		else
			pos_adj = ((pos_adj + pos_align - 1) / pos_align) *
				pos_align;
		pos_adj = frames_to_bytes(runtime, pos_adj);
		if (pos_adj >= period_bytes) {
			snd_printk(KERN_WARNING SFX "Too big adjustment %d\n",
				   bdl_pos_adj[chip->dev_index]);
			pos_adj = 0;
		} else {
			ofs = setup_bdle(substream, azx_dev,
					 &bdl, ofs, pos_adj,
					 !substream->runtime->no_period_wakeup);
			if (ofs < 0)
				goto error;
		}
	} else
		pos_adj = 0;
	for (i = 0; i < periods; i++) {
		if (i == periods - 1 && pos_adj)
			ofs = setup_bdle(substream, azx_dev, &bdl, ofs,
					 period_bytes - pos_adj, 0);
		else
			ofs = setup_bdle(substream, azx_dev, &bdl, ofs,
					 period_bytes,
					 !substream->runtime->no_period_wakeup);
		if (ofs < 0)
			goto error;
	}
	return 0;

 error:
	snd_printk(KERN_ERR SFX "Too many BDL entries: buffer=%d, period=%d\n",
		   azx_dev->bufsize, period_bytes);
	return -EINVAL;
}

/* reset stream */
static void azx_stream_reset(struct azx *chip, struct azx_dev *azx_dev)
{
	unsigned char val;
	int timeout;

	azx_stream_clear(chip, azx_dev);

	azx_sd_writeb(azx_dev, SD_CTL, azx_sd_readb(azx_dev, SD_CTL) |
		      SD_CTL_STREAM_RESET);
	udelay(3);
	timeout = 300;
	while (!((val = azx_sd_readb(azx_dev, SD_CTL)) & SD_CTL_STREAM_RESET) &&
	       --timeout)
		;
	val &= ~SD_CTL_STREAM_RESET;
	azx_sd_writeb(azx_dev, SD_CTL, val);
	udelay(3);

	timeout = 300;
	/* waiting for hardware to report that the stream is out of reset */
	while (((val = azx_sd_readb(azx_dev, SD_CTL)) & SD_CTL_STREAM_RESET) &&
	       --timeout)
		;

	/* reset first position - may not be synced with hw at this time */
	*azx_dev->posbuf = 0;
}

/*
 * set up the SD for streaming
 */
static int azx_setup_controller(struct azx *chip, struct azx_dev *azx_dev)
{
	unsigned int val;
	/* make sure the run bit is zero for SD */
	azx_stream_clear(chip, azx_dev);
	/* program the stream_tag */
	val = azx_sd_readl(azx_dev, SD_CTL);
	val = (val & ~SD_CTL_STREAM_TAG_MASK) |
		(azx_dev->stream_tag << SD_CTL_STREAM_TAG_SHIFT);
	if (!azx_snoop(chip))
		val |= SD_CTL_TRAFFIC_PRIO;
	azx_sd_writel(azx_dev, SD_CTL, val);

	/* program the length of samples in cyclic buffer */
	azx_sd_writel(azx_dev, SD_CBL, azx_dev->bufsize);

	/* program the stream format */
	/* this value needs to be the same as the one programmed */
	azx_sd_writew(azx_dev, SD_FORMAT, azx_dev->format_val);

	/* program the stream LVI (last valid index) of the BDL */
	azx_sd_writew(azx_dev, SD_LVI, azx_dev->frags - 1);

	/* program the BDL address */
	/* lower BDL address */
	azx_sd_writel(azx_dev, SD_BDLPL, (u32)azx_dev->bdl.addr);
	/* upper BDL address */
	azx_sd_writel(azx_dev, SD_BDLPU, upper_32_bits(azx_dev->bdl.addr));

	/* enable the position buffer */
	if (chip->position_fix[0] != POS_FIX_LPIB ||
	    chip->position_fix[1] != POS_FIX_LPIB) {
		if (!(azx_readl(chip, DPLBASE) & ICH6_DPLBASE_ENABLE))
			azx_writel(chip, DPLBASE,
				(u32)chip->posbuf.addr | ICH6_DPLBASE_ENABLE);
	}

	/* set the interrupt enable bits in the descriptor control register */
	azx_sd_writel(azx_dev, SD_CTL,
		      azx_sd_readl(azx_dev, SD_CTL) | SD_INT_MASK);

	return 0;
}

/*
 * Probe the given codec address
 */
static int probe_codec(struct azx *chip, int addr)
{
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	unsigned int res;

	mutex_lock(&chip->bus->cmd_mutex);
	chip->probing = 1;
	azx_send_cmd(chip->bus, cmd);
	res = azx_get_response(chip->bus, addr);
	chip->probing = 0;
	mutex_unlock(&chip->bus->cmd_mutex);
	if (res == -1)
		return -EIO;
	snd_printdd(SFX "codec #%d probed OK\n", addr);
	return 0;
}

static int azx_attach_pcm_stream(struct hda_bus *bus, struct hda_codec *codec,
				 struct hda_pcm *cpcm);
static void azx_stop_chip(struct azx *chip);

static void azx_bus_reset(struct hda_bus *bus)
{
	struct azx *chip = bus->private_data;

	bus->in_reset = 1;
	azx_stop_chip(chip);
	azx_init_chip(chip, 1);
#ifdef CONFIG_PM
	if (chip->initialized) {
		struct azx_pcm *p;
		list_for_each_entry(p, &chip->pcm_list, list)
			snd_pcm_suspend_all(p->pcm);
		snd_hda_suspend(chip->bus);
		snd_hda_resume(chip->bus);
	}
#endif
	bus->in_reset = 0;
}

/*
 * Codec initialization
 */

/* number of codec slots for each chipset: 0 = default slots (i.e. 4) */
static unsigned int azx_max_codecs[AZX_NUM_DRIVERS] __devinitdata = {
	[AZX_DRIVER_NVIDIA] = 8,
	[AZX_DRIVER_TERA] = 1,
};

static int __devinit azx_codec_create(struct azx *chip, const char *model)
{
	struct hda_bus_template bus_temp;
	int c, codecs, err;
	int max_slots;

	memset(&bus_temp, 0, sizeof(bus_temp));
	bus_temp.private_data = chip;
	bus_temp.modelname = model;
	bus_temp.pci = chip->pci;
	bus_temp.pdev = chip->pdev;
	bus_temp.ops.command = azx_send_cmd;
	bus_temp.ops.get_response = azx_get_response;
	bus_temp.ops.attach_pcm = azx_attach_pcm_stream;
	bus_temp.ops.bus_reset = azx_bus_reset;
#ifdef CONFIG_SND_HDA_POWER_SAVE
	bus_temp.power_save = &power_save;
	bus_temp.ops.pm_notify = azx_power_notify;
#endif

	err = snd_hda_bus_new(chip->card, &bus_temp, &chip->bus);
	if (err < 0)
		return err;

	if (chip->driver_caps & AZX_DCAPS_RIRB_DELAY) {
		snd_printd(SFX "Enable delay in RIRB handling\n");
		chip->bus->needs_damn_long_delay = 1;
	}

	codecs = 0;
	max_slots = azx_max_codecs[chip->driver_type];
	if (!max_slots)
		max_slots = AZX_DEFAULT_CODECS;

	/* First try to probe all given codec slots */
	for (c = 0; c < max_slots; c++) {
		if ((chip->codec_mask & (1 << c)) & chip->codec_probe_mask) {
			if (probe_codec(chip, c) < 0) {
				/* Some BIOSen give you wrong codec addresses
				 * that don't exist
				 */
				snd_printk(KERN_WARNING SFX
					   "Codec #%d probe error; "
					   "disabling it...\n", c);
				chip->codec_mask &= ~(1 << c);
				/* More badly, accessing to a non-existing
				 * codec often screws up the controller chip,
				 * and disturbs the further communications.
				 * Thus if an error occurs during probing,
				 * better to reset the controller chip to
				 * get back to the sanity state.
				 */
				azx_stop_chip(chip);
				azx_init_chip(chip, 1);
			}
		}
	}

	/* AMD chipsets often cause the communication stalls upon certain
	 * sequence like the pin-detection.  It seems that forcing the synced
	 * access works around the stall.  Grrr...
	 */
	if (chip->driver_caps & AZX_DCAPS_SYNC_WRITE) {
		snd_printd(SFX "Enable sync_write for stable communication\n");
		chip->bus->sync_write = 1;
		chip->bus->allow_bus_reset = 1;
	}

	/* Then create codec instances */
	for (c = 0; c < max_slots; c++) {
		if ((chip->codec_mask & (1 << c)) & chip->codec_probe_mask) {
			struct hda_codec *codec;
			err = snd_hda_codec_new(chip->bus, c, &codec);
			if (err < 0)
				continue;
			codec->beep_mode = chip->beep_mode;
			codecs++;
		}
	}
	if (!codecs) {
		snd_printk(KERN_ERR SFX "no codecs initialized\n");
		return -ENXIO;
	}
	return 0;
}

/* configure each codec instance */
static int __devinit azx_codec_configure(struct azx *chip)
{
	struct hda_codec *codec;
	list_for_each_entry(codec, &chip->bus->codec_list, list) {
		snd_hda_codec_configure(codec);
	}
	return 0;
}


/*
 * PCM support
 */

/* assign a stream for the PCM */
static inline struct azx_dev *
azx_assign_device(struct azx *chip, struct snd_pcm_substream *substream)
{
	int dev, i, nums;
	struct azx_dev *res = NULL;
	/* make a non-zero unique key for the substream */
	int key = (substream->pcm->device << 16) | (substream->number << 2) |
		(substream->stream + 1);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dev = chip->playback_index_offset;
		nums = chip->playback_streams;
	} else {
		dev = chip->capture_index_offset;
		nums = chip->capture_streams;
	}
	for (i = 0; i < nums; i++, dev++)
		if (!chip->azx_dev[dev].opened) {
			res = &chip->azx_dev[dev];
			if (res->assigned_key == key)
				break;
		}
	if (res) {
		res->opened = 1;
		res->assigned_key = key;
	}
	return res;
}

/* release the assigned stream */
static inline void azx_release_device(struct azx_dev *azx_dev)
{
	azx_dev->opened = 0;
}

static struct snd_pcm_hardware azx_pcm_hw = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |
				 SNDRV_PCM_INFO_MMAP_VALID |
				 /* No full-resume yet implemented */
				 /* SNDRV_PCM_INFO_RESUME |*/
				 SNDRV_PCM_INFO_PAUSE |
				 SNDRV_PCM_INFO_SYNC_START |
				 SNDRV_PCM_INFO_NO_PERIOD_WAKEUP),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_48000,
	.rate_min =		48000,
	.rate_max =		48000,
	.channels_min =		2,
	.channels_max =		2,
	.buffer_bytes_max =	AZX_MAX_BUF_SIZE,
	.period_bytes_min =	128,
	.period_bytes_max =	AZX_MAX_BUF_SIZE / 2,
	.periods_min =		2,
	.periods_max =		AZX_MAX_FRAG,
	.fifo_size =		0,
};

static int azx_pcm_open(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct hda_pcm_stream *hinfo = apcm->hinfo[substream->stream];
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int err;
	int buff_step;

	mutex_lock(&chip->open_mutex);
	azx_dev = azx_assign_device(chip, substream);
	if (azx_dev == NULL) {
		mutex_unlock(&chip->open_mutex);
		return -EBUSY;
	}
	runtime->hw = azx_pcm_hw;
	runtime->hw.channels_min = hinfo->channels_min;
	runtime->hw.channels_max = hinfo->channels_max;
	runtime->hw.formats = hinfo->formats;
	runtime->hw.rates = hinfo->rates;
	snd_pcm_limit_hw_rates(runtime);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (chip->align_buffer_size)
		/* constrain buffer sizes to be multiple of 128
		   bytes. This is more efficient in terms of memory
		   access but isn't required by the HDA spec and
		   prevents users from specifying exact period/buffer
		   sizes. For example for 44.1kHz, a period size set
		   to 20ms will be rounded to 19.59ms. */
		buff_step = 128;
	else
		/* Don't enforce steps on buffer sizes, still need to
		   be multiple of 4 bytes (HDA spec). Tested on Intel
		   HDA controllers, may not work on all devices where
		   option needs to be disabled */
		buff_step = 4;

	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				   buff_step);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   buff_step);
	snd_hda_power_up(apcm->codec);
	err = hinfo->ops.open(hinfo, apcm->codec, substream);
	if (err < 0) {
		azx_release_device(azx_dev);
		snd_hda_power_down(apcm->codec);
		mutex_unlock(&chip->open_mutex);
		return err;
	}
	snd_pcm_limit_hw_rates(runtime);
	/* sanity check */
	if (snd_BUG_ON(!runtime->hw.channels_min) ||
	    snd_BUG_ON(!runtime->hw.channels_max) ||
	    snd_BUG_ON(!runtime->hw.formats) ||
	    snd_BUG_ON(!runtime->hw.rates)) {
		azx_release_device(azx_dev);
		hinfo->ops.close(hinfo, apcm->codec, substream);
		snd_hda_power_down(apcm->codec);
		mutex_unlock(&chip->open_mutex);
		return -EINVAL;
	}
	spin_lock_irqsave(&chip->reg_lock, flags);
	azx_dev->substream = substream;
	azx_dev->running = 0;
	spin_unlock_irqrestore(&chip->reg_lock, flags);

	runtime->private_data = azx_dev;
	snd_pcm_set_sync(substream);
	mutex_unlock(&chip->open_mutex);
	return 0;
}

static int azx_pcm_close(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct hda_pcm_stream *hinfo = apcm->hinfo[substream->stream];
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev = get_azx_dev(substream);
	unsigned long flags;

	mutex_lock(&chip->open_mutex);
	spin_lock_irqsave(&chip->reg_lock, flags);
	azx_dev->substream = NULL;
	azx_dev->running = 0;
	spin_unlock_irqrestore(&chip->reg_lock, flags);
	azx_release_device(azx_dev);
	hinfo->ops.close(hinfo, apcm->codec, substream);
	snd_hda_power_down(apcm->codec);
	mutex_unlock(&chip->open_mutex);
	return 0;
}

static int azx_pcm_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *hw_params)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev = get_azx_dev(substream);
	int ret;

	mark_runtime_wc(chip, azx_dev, substream, false);
	azx_dev->bufsize = 0;
	azx_dev->period_bytes = 0;
	azx_dev->format_val = 0;
	ret = snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	mark_runtime_wc(chip, azx_dev, substream, true);
	return ret;
}

static int azx_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx_dev *azx_dev = get_azx_dev(substream);
	struct azx *chip = apcm->chip;
	struct hda_pcm_stream *hinfo = apcm->hinfo[substream->stream];

	/* reset BDL address */
	azx_sd_writel(azx_dev, SD_BDLPL, 0);
	azx_sd_writel(azx_dev, SD_BDLPU, 0);
	azx_sd_writel(azx_dev, SD_CTL, 0);
	azx_dev->bufsize = 0;
	azx_dev->period_bytes = 0;
	azx_dev->format_val = 0;

	snd_hda_codec_cleanup(apcm->codec, hinfo, substream);

	mark_runtime_wc(chip, azx_dev, substream, false);
	return snd_pcm_lib_free_pages(substream);
}

static int azx_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev = get_azx_dev(substream);
	struct hda_pcm_stream *hinfo = apcm->hinfo[substream->stream];
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int bufsize, period_bytes, format_val, stream_tag;
	int err;
	struct hda_spdif_out *spdif =
		snd_hda_spdif_out_of_nid(apcm->codec, hinfo->nid);
	unsigned short ctls = spdif ? spdif->ctls : 0;

	azx_stream_reset(chip, azx_dev);
	format_val = snd_hda_calc_stream_format(runtime->rate,
						runtime->channels,
						runtime->format,
						hinfo->maxbps,
						ctls);
	if (!format_val) {
		snd_printk(KERN_ERR SFX
			   "invalid format_val, rate=%d, ch=%d, format=%d\n",
			   runtime->rate, runtime->channels, runtime->format);
		return -EINVAL;
	}

	bufsize = snd_pcm_lib_buffer_bytes(substream);
	period_bytes = snd_pcm_lib_period_bytes(substream);

	snd_printdd(SFX "azx_pcm_prepare: bufsize=0x%x, format=0x%x\n",
		    bufsize, format_val);

	if (bufsize != azx_dev->bufsize ||
	    period_bytes != azx_dev->period_bytes ||
	    format_val != azx_dev->format_val) {
		azx_dev->bufsize = bufsize;
		azx_dev->period_bytes = period_bytes;
		azx_dev->format_val = format_val;
		err = azx_setup_periods(chip, substream, azx_dev);
		if (err < 0)
			return err;
	}

	/* wallclk has 24Mhz clock source */
	azx_dev->period_wallclk = (((runtime->period_size * 24000) /
						runtime->rate) * 1000);
	azx_setup_controller(chip, azx_dev);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		azx_dev->fifo_size = azx_sd_readw(azx_dev, SD_FIFOSIZE) + 1;
	else
		azx_dev->fifo_size = 0;

	stream_tag = azx_dev->stream_tag;
	/* CA-IBG chips need the playback stream starting from 1 */
	if ((chip->driver_caps & AZX_DCAPS_CTX_WORKAROUND) &&
	    stream_tag > chip->capture_streams)
		stream_tag -= chip->capture_streams;
	return snd_hda_codec_prepare(apcm->codec, hinfo, stream_tag,
				     azx_dev->format_val, substream);
}

static int azx_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev;
	struct snd_pcm_substream *s;
	int rstart = 0, start, nsync = 0, sbits = 0;
	int nwait, timeout;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		rstart = 1;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		start = 1;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		start = 0;
		break;
	default:
		return -EINVAL;
	}

	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		azx_dev = get_azx_dev(s);
		sbits |= 1 << azx_dev->index;
		nsync++;
		snd_pcm_trigger_done(s, substream);
	}

	spin_lock(&chip->reg_lock);
	if (nsync > 1) {
		/* first, set SYNC bits of corresponding streams */
		if (chip->driver_caps & AZX_DCAPS_OLD_SSYNC)
			azx_writel(chip, OLD_SSYNC,
				   azx_readl(chip, OLD_SSYNC) | sbits);
		else
			azx_writel(chip, SSYNC, azx_readl(chip, SSYNC) | sbits);
	}
	snd_pcm_group_for_each_entry(s, substream) {
		if (s->pcm->card != substream->pcm->card)
			continue;
		azx_dev = get_azx_dev(s);
		if (start) {
			azx_dev->start_wallclk = azx_readl(chip, WALLCLK);
			if (!rstart)
				azx_dev->start_wallclk -=
						azx_dev->period_wallclk;
			azx_stream_start(chip, azx_dev);
		} else {
			azx_stream_stop(chip, azx_dev);
		}
		azx_dev->running = start;
	}
	spin_unlock(&chip->reg_lock);
	if (start) {
		if (nsync == 1)
			return 0;
		/* wait until all FIFOs get ready */
		for (timeout = 5000; timeout; timeout--) {
			nwait = 0;
			snd_pcm_group_for_each_entry(s, substream) {
				if (s->pcm->card != substream->pcm->card)
					continue;
				azx_dev = get_azx_dev(s);
				if (!(azx_sd_readb(azx_dev, SD_STS) &
				      SD_STS_FIFO_READY))
					nwait++;
			}
			if (!nwait)
				break;
			cpu_relax();
		}
	} else {
		/* wait until all RUN bits are cleared */
		for (timeout = 5000; timeout; timeout--) {
			nwait = 0;
			snd_pcm_group_for_each_entry(s, substream) {
				if (s->pcm->card != substream->pcm->card)
					continue;
				azx_dev = get_azx_dev(s);
				if (azx_sd_readb(azx_dev, SD_CTL) &
				    SD_CTL_DMA_START)
					nwait++;
			}
			if (!nwait)
				break;
			cpu_relax();
		}
	}
	if (nsync > 1) {
		spin_lock(&chip->reg_lock);
		/* reset SYNC bits */
		if (chip->driver_caps & AZX_DCAPS_OLD_SSYNC)
			azx_writel(chip, OLD_SSYNC,
				   azx_readl(chip, OLD_SSYNC) & ~sbits);
		else
			azx_writel(chip, SSYNC, azx_readl(chip, SSYNC) & ~sbits);
		spin_unlock(&chip->reg_lock);
	}
	return 0;
}

/* get the current DMA position with correction on VIA chips */
static unsigned int azx_via_get_position(struct azx *chip,
					 struct azx_dev *azx_dev)
{
	unsigned int link_pos, mini_pos, bound_pos;
	unsigned int mod_link_pos, mod_dma_pos, mod_mini_pos;
	unsigned int fifo_size;

	link_pos = azx_sd_readl(azx_dev, SD_LPIB);
	if (azx_dev->substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Playback, no problem using link position */
		return link_pos;
	}

	/* Capture */
	/* For new chipset,
	 * use mod to get the DMA position just like old chipset
	 */
	mod_dma_pos = le32_to_cpu(*azx_dev->posbuf);
	mod_dma_pos %= azx_dev->period_bytes;

	/* azx_dev->fifo_size can't get FIFO size of in stream.
	 * Get from base address + offset.
	 */
	fifo_size = readw(chip->remap_addr + VIA_IN_STREAM0_FIFO_SIZE_OFFSET);

	if (azx_dev->insufficient) {
		/* Link position never gather than FIFO size */
		if (link_pos <= fifo_size)
			return 0;

		azx_dev->insufficient = 0;
	}

	if (link_pos <= fifo_size)
		mini_pos = azx_dev->bufsize + link_pos - fifo_size;
	else
		mini_pos = link_pos - fifo_size;

	/* Find nearest previous boudary */
	mod_mini_pos = mini_pos % azx_dev->period_bytes;
	mod_link_pos = link_pos % azx_dev->period_bytes;
	if (mod_link_pos >= fifo_size)
		bound_pos = link_pos - mod_link_pos;
	else if (mod_dma_pos >= mod_mini_pos)
		bound_pos = mini_pos - mod_mini_pos;
	else {
		bound_pos = mini_pos - mod_mini_pos + azx_dev->period_bytes;
		if (bound_pos >= azx_dev->bufsize)
			bound_pos = 0;
	}

	/* Calculate real DMA position we want */
	return bound_pos + mod_dma_pos;
}

static unsigned int azx_get_position(struct azx *chip,
				     struct azx_dev *azx_dev,
				     bool with_check)
{
	unsigned int pos;
	int stream = azx_dev->substream->stream;

	switch (chip->position_fix[stream]) {
	case POS_FIX_LPIB:
		/* read LPIB */
		pos = azx_sd_readl(azx_dev, SD_LPIB);
		break;
	case POS_FIX_VIACOMBO:
		pos = azx_via_get_position(chip, azx_dev);
		break;
	default:
		/* use the position buffer */
		pos = le32_to_cpu(*azx_dev->posbuf);
		if (with_check && chip->position_fix[stream] == POS_FIX_AUTO) {
			if (!pos || pos == (u32)-1) {
				printk(KERN_WARNING
				       "hda-intel: Invalid position buffer, "
				       "using LPIB read method instead.\n");
				chip->position_fix[stream] = POS_FIX_LPIB;
				pos = azx_sd_readl(azx_dev, SD_LPIB);
			} else
				chip->position_fix[stream] = POS_FIX_POSBUF;
		}
		break;
	}

	if (pos >= azx_dev->bufsize)
		pos = 0;
	return pos;
}

static snd_pcm_uframes_t azx_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx *chip = apcm->chip;
	struct azx_dev *azx_dev = get_azx_dev(substream);
	return bytes_to_frames(substream->runtime,
			       azx_get_position(chip, azx_dev, false));
}

/*
 * Check whether the current DMA position is acceptable for updating
 * periods.  Returns non-zero if it's OK.
 *
 * Many HD-audio controllers appear pretty inaccurate about
 * the update-IRQ timing.  The IRQ is issued before actually the
 * data is processed.  So, we need to process it afterwords in a
 * workqueue.
 */
static int azx_position_ok(struct azx *chip, struct azx_dev *azx_dev)
{
	u32 wallclk;
	unsigned int pos;
	int stream;

	wallclk = azx_readl(chip, WALLCLK) - azx_dev->start_wallclk;
	if (wallclk < (azx_dev->period_wallclk * 2) / 3)
		return -1;	/* bogus (too early) interrupt */

	stream = azx_dev->substream->stream;
	pos = azx_get_position(chip, azx_dev, true);

	if (WARN_ONCE(!azx_dev->period_bytes,
		      "hda-intel: zero azx_dev->period_bytes"))
		return -1; /* this shouldn't happen! */
	if (wallclk < (azx_dev->period_wallclk * 5) / 4 &&
	    pos % azx_dev->period_bytes > azx_dev->period_bytes / 2)
		/* NG - it's below the first next period boundary */
		return bdl_pos_adj[chip->dev_index] ? 0 : -1;
	azx_dev->start_wallclk += wallclk;
	return 1; /* OK, it's fine */
}

/*
 * The work for pending PCM period updates.
 */
static void azx_irq_pending_work(struct work_struct *work)
{
	struct azx *chip = container_of(work, struct azx, irq_pending_work);
	int i, pending, ok;

	if (!chip->irq_pending_warned) {
		printk(KERN_WARNING
		       "hda-intel: IRQ timing workaround is activated "
		       "for card #%d. Suggest a bigger bdl_pos_adj.\n",
		       chip->card->number);
		chip->irq_pending_warned = 1;
	}

	for (;;) {
		pending = 0;
		spin_lock_irq(&chip->reg_lock);
		for (i = 0; i < chip->num_streams; i++) {
			struct azx_dev *azx_dev = &chip->azx_dev[i];
			if (!azx_dev->irq_pending ||
			    !azx_dev->substream ||
			    !azx_dev->running)
				continue;
			ok = azx_position_ok(chip, azx_dev);
			if (ok > 0) {
				azx_dev->irq_pending = 0;
				spin_unlock(&chip->reg_lock);
				snd_pcm_period_elapsed(azx_dev->substream);
				spin_lock(&chip->reg_lock);
			} else if (ok < 0) {
				pending = 0;	/* too early */
			} else
				pending++;
		}
		spin_unlock_irq(&chip->reg_lock);
		if (!pending)
			return;
		msleep(1);
	}
}

/* clear irq_pending flags and assure no on-going workq */
static void azx_clear_irq_pending(struct azx *chip)
{
	int i;

	spin_lock_irq(&chip->reg_lock);
	for (i = 0; i < chip->num_streams; i++)
		chip->azx_dev[i].irq_pending = 0;
	spin_unlock_irq(&chip->reg_lock);
}

#ifdef CONFIG_X86
static int azx_pcm_mmap(struct snd_pcm_substream *substream,
			struct vm_area_struct *area)
{
	struct azx_pcm *apcm = snd_pcm_substream_chip(substream);
	struct azx *chip = apcm->chip;
	if (!azx_snoop(chip))
		area->vm_page_prot = pgprot_writecombine(area->vm_page_prot);
	return snd_pcm_lib_default_mmap(substream, area);
}
#else
#define azx_pcm_mmap	NULL
#endif

static struct snd_pcm_ops azx_pcm_ops = {
	.open = azx_pcm_open,
	.close = azx_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = azx_pcm_hw_params,
	.hw_free = azx_pcm_hw_free,
	.prepare = azx_pcm_prepare,
	.trigger = azx_pcm_trigger,
	.pointer = azx_pcm_pointer,
	.mmap = azx_pcm_mmap,
	.page = snd_pcm_sgbuf_ops_page,
};

static void azx_pcm_free(struct snd_pcm *pcm)
{
	struct azx_pcm *apcm = pcm->private_data;
	if (apcm) {
		list_del(&apcm->list);
		kfree(apcm);
	}
}

#define MAX_PREALLOC_SIZE	(32 * 1024 * 1024)

static int
azx_attach_pcm_stream(struct hda_bus *bus, struct hda_codec *codec,
		      struct hda_pcm *cpcm)
{
	struct azx *chip = bus->private_data;
	struct snd_pcm *pcm;
	struct azx_pcm *apcm;
	int pcm_dev = cpcm->device;
	unsigned int size;
	int s, err;

	list_for_each_entry(apcm, &chip->pcm_list, list) {
		if (apcm->pcm->device == pcm_dev) {
			snd_printk(KERN_ERR SFX "PCM %d already exists\n", pcm_dev);
			return -EBUSY;
		}
	}
	err = snd_pcm_new(chip->card, cpcm->name, pcm_dev,
			  cpcm->stream[SNDRV_PCM_STREAM_PLAYBACK].substreams,
			  cpcm->stream[SNDRV_PCM_STREAM_CAPTURE].substreams,
			  &pcm);
	if (err < 0)
		return err;
	strlcpy(pcm->name, cpcm->name, sizeof(pcm->name));
	apcm = kzalloc(sizeof(*apcm), GFP_KERNEL);
	if (apcm == NULL)
		return -ENOMEM;
	apcm->chip = chip;
	apcm->pcm = pcm;
	apcm->codec = codec;
	pcm->private_data = apcm;
	pcm->private_free = azx_pcm_free;
	if (cpcm->pcm_type == HDA_PCM_TYPE_MODEM)
		pcm->dev_class = SNDRV_PCM_CLASS_MODEM;
	list_add_tail(&apcm->list, &chip->pcm_list);
	cpcm->pcm = pcm;
	for (s = 0; s < 2; s++) {
		apcm->hinfo[s] = &cpcm->stream[s];
		if (cpcm->stream[s].substreams)
			snd_pcm_set_ops(pcm, s, &azx_pcm_ops);
	}
	/* buffer pre-allocation */
	size = CONFIG_SND_HDA_PREALLOC_SIZE * 1024;
	if (size > MAX_PREALLOC_SIZE)
		size = MAX_PREALLOC_SIZE;
#ifdef CONFIG_SND_HDA_VPR
	chip->hda_vpr = nvmap_create_client(nvmap_dev, "hda_vpr");
	for (s = 0; s < 2; s++) {
		struct snd_pcm_substream *substream;
		for (substream = pcm->streams[s].substream;
		  substream; substream = substream->next) {
			chip->handle_ref = nvmap_alloc(chip->hda_vpr, size, 32,
			  NVMAP_HANDLE_WRITE_COMBINE, NVMAP_HEAP_CARVEOUT_VPR);
			chip->vaddr =
			  (unsigned char *) nvmap_mmap(chip->handle_ref);
			chip->paddr =
			  nvmap_pin(chip->hda_vpr, chip->handle_ref);
			snd_printk(KERN_DEBUG SFX
			  "paddr=%08x vaddr=%08x\n", chip->paddr, chip->vaddr);
			substream->dma_buffer.area = chip->vaddr;
			substream->dma_buffer.addr = chip->paddr;
			substream->dma_buffer.bytes = size;
			substream->dma_buffer.dev.dev = chip->dev;
			if (substream->dma_buffer.bytes > 0)
				substream->buffer_bytes_max =
				   substream->dma_buffer.bytes;
			substream->dma_max = MAX_PREALLOC_SIZE;
		}
	}
#else
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV_SG,
					      chip->dev,
					      size, MAX_PREALLOC_SIZE);
#endif
	return 0;
}

/*
 * mixer creation - all stuff is implemented in hda module
 */
static int __devinit azx_mixer_create(struct azx *chip)
{
	return snd_hda_build_controls(chip->bus);
}


/*
 * initialize SD streams
 */
static int __devinit azx_init_stream(struct azx *chip)
{
	int i;

	/* initialize each stream (aka device)
	 * assign the starting bdl address to each stream (device)
	 * and initialize
	 */
	for (i = 0; i < chip->num_streams; i++) {
		struct azx_dev *azx_dev = &chip->azx_dev[i];
		azx_dev->posbuf = (u32 __iomem *)(chip->posbuf.area + i * 8);
		/* offset: SDI0=0x80, SDI1=0xa0, ... SDO3=0x160 */
		azx_dev->sd_addr = chip->remap_addr + (0x20 * i + 0x80);
		/* int mask: SDI0=0x01, SDI1=0x02, ... SDO3=0x80 */
		azx_dev->sd_int_sta_mask = 1 << i;
		/* stream tag: must be non-zero and unique */
		azx_dev->index = i;
		azx_dev->stream_tag = i + 1;
	}

	return 0;
}

static int azx_acquire_irq(struct azx *chip, int do_disconnect)
{
	if (request_irq(chip->irq_id, azx_interrupt,
			chip->msi ? 0 : IRQF_SHARED,
			KBUILD_MODNAME, chip)) {
		printk(KERN_ERR "hda-intel: unable to grab IRQ %d, "
		       "disabling device\n", chip->irq_id);
		if (do_disconnect)
			snd_card_disconnect(chip->card);
		return -1;
	}
	chip->irq = chip->irq_id;
	if (chip->pci)
		pci_intx(chip->pci, !chip->msi);

	return 0;
}


static void azx_stop_chip(struct azx *chip)
{
	if (!chip->initialized)
		return;

	/* disable interrupts */
	azx_int_disable(chip);
	azx_int_clear(chip);

	/* disable CORB/RIRB */
	azx_free_cmd_io(chip);

	/* disable position buffer */
	azx_writel(chip, DPLBASE, 0);
	azx_writel(chip, DPUBASE, 0);

	chip->initialized = 0;
}

#ifdef CONFIG_SND_HDA_POWER_SAVE
/* power-up/down the controller */
static void azx_power_notify(struct hda_bus *bus)
{
	struct azx *chip = bus->private_data;
	struct hda_codec *c;
	int power_on = 0;

	list_for_each_entry(c, &bus->codec_list, list) {
		if (c->power_on) {
			power_on = 1;
			break;
		}
	}
	if (power_on) {
#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
		azx_platform_enable_clocks(chip);
#endif
		azx_init_chip(chip, 1);
	}
	else if (chip->running && power_save_controller &&
		 !bus->power_keep_link_on) {
		azx_stop_chip(chip);
#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
		azx_platform_disable_clocks(chip);
#endif
	}
}
#endif /* CONFIG_SND_HDA_POWER_SAVE */

#ifdef CONFIG_PM
/*
 * power management
 */

static int snd_hda_codecs_inuse(struct hda_bus *bus)
{
	struct hda_codec *codec;

	list_for_each_entry(codec, &bus->codec_list, list) {
		if (snd_hda_codec_needs_resume(codec))
			return 1;
	}
	return 0;
}

static int azx_suspend(struct azx *chip, pm_message_t state)
{
	struct snd_card *card = chip->card;
	struct azx_pcm *p;

#if defined(CONFIG_SND_HDA_PLATFORM_DRIVER) && \
	defined(CONFIG_SND_HDA_POWER_SAVE)
	if (chip->pdev)
		azx_platform_enable_clocks(chip);
#endif

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	azx_clear_irq_pending(chip);
	list_for_each_entry(p, &chip->pcm_list, list)
		snd_pcm_suspend_all(p->pcm);
	if (chip->initialized)
		snd_hda_suspend(chip->bus);
	azx_stop_chip(chip);
	if (chip->irq >= 0) {
		free_irq(chip->irq, chip);
		chip->irq = -1;
	}

	if (chip->pci) {
		if (chip->msi)
			pci_disable_msi(chip->pci);
		pci_disable_device(chip->pci);
		pci_save_state(chip->pci);
		pci_set_power_state(chip->pci,
				    pci_choose_state(chip->pci, state));
	}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	if (chip->pdev) {
		/* Disable all clk references */
		while (chip->platform_clk_enable)
			azx_platform_disable_clocks(chip);
	}
#endif

	return 0;
}

static int azx_resume(struct azx *chip)
{
	struct snd_card *card = chip->card;

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	if (chip->pdev)
		azx_platform_enable_clocks(chip);
#endif

	if (chip->pci) {
		pci_set_power_state(chip->pci, PCI_D0);
		pci_restore_state(chip->pci);
		if (pci_enable_device(chip->pci) < 0) {
			printk(KERN_ERR "hda-intel: pci_enable_device failed, "
			       "disabling device\n");
			snd_card_disconnect(card);
			return -EIO;
		}
		pci_set_master(chip->pci);
		if (chip->msi)
			if (pci_enable_msi(chip->pci) < 0)
				chip->msi = 0;
	}

	if (azx_acquire_irq(chip, 1) < 0)
		return -EIO;

	if (chip->pci)
		azx_init_pci(chip);

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	if (chip->pdev)
		azx_init_platform(chip);
#endif

	if (snd_hda_codecs_inuse(chip->bus))
		azx_init_chip(chip, 1);

	snd_hda_resume(chip->bus);
	snd_power_change_state(card, SNDRV_CTL_POWER_D0);

#if defined(CONFIG_SND_HDA_PLATFORM_DRIVER) && \
	defined(CONFIG_SND_HDA_POWER_SAVE)
	if (chip->pdev)
		azx_platform_disable_clocks(chip);
#endif

	return 0;
}

static int azx_suspend_pci(struct pci_dev *pci, pm_message_t state)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct azx *chip = card->private_data;

	return azx_suspend(chip, state);
}

static int azx_resume_pci(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct azx *chip = card->private_data;

	return azx_resume(chip);
}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
static int azx_suspend_platform(struct platform_device *pdev,
				pm_message_t state)
{
	struct snd_card *card = dev_get_drvdata(&pdev->dev);
	struct azx *chip = card->private_data;

	return azx_suspend(chip, state);
}

static int azx_resume_platform(struct platform_device *pdev)
{
	struct snd_card *card = dev_get_drvdata(&pdev->dev);
	struct azx *chip = card->private_data;

	return azx_resume(chip);
}
#endif /* CONFIG_SND_HDA_PLATFORM_DRIVER */
#endif /* CONFIG_PM */


/*
 * reboot notifier for hang-up problem at power-down
 */
static int azx_halt(struct notifier_block *nb, unsigned long event, void *buf)
{
	struct azx *chip = container_of(nb, struct azx, reboot_notifier);

#if defined(CONFIG_SND_HDA_PLATFORM_DRIVER) && \
	defined(CONFIG_SND_HDA_POWER_SAVE)
	if (chip->pdev)
		azx_platform_enable_clocks(chip);
#endif

	snd_hda_bus_reboot_notify(chip->bus);
	azx_stop_chip(chip);

#if defined(CONFIG_SND_HDA_PLATFORM_DRIVER) && \
	defined(CONFIG_SND_HDA_POWER_SAVE)
	if (chip->pdev)
		azx_platform_disable_clocks(chip);
#endif

	return NOTIFY_OK;
}

static void azx_notifier_register(struct azx *chip)
{
	chip->reboot_notifier.notifier_call = azx_halt;
	register_reboot_notifier(&chip->reboot_notifier);
}

static void azx_notifier_unregister(struct azx *chip)
{
	if (chip->reboot_notifier.notifier_call)
		unregister_reboot_notifier(&chip->reboot_notifier);
}

/*
 * destructor
 */
static int azx_free(struct azx *chip)
{
	int i;

	azx_notifier_unregister(chip);

	if (chip->initialized) {
		azx_clear_irq_pending(chip);
		for (i = 0; i < chip->num_streams; i++)
			azx_stream_stop(chip, &chip->azx_dev[i]);
		azx_stop_chip(chip);
	}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	azx_platform_disable_clocks(chip);
	for (i = 0; i < chip->platform_clk_count; i++)
		clk_put(chip->platform_clks[i]);
#endif

	if (chip->irq >= 0)
		free_irq(chip->irq, (void*)chip);
	if (chip->pci && chip->msi)
		pci_disable_msi(chip->pci);
	if (chip->remap_addr)
		iounmap(chip->remap_addr);

	if (chip->azx_dev) {
		for (i = 0; i < chip->num_streams; i++)
			if (chip->azx_dev[i].bdl.area) {
				mark_pages_wc(chip, &chip->azx_dev[i].bdl, false);
				snd_dma_free_pages(&chip->azx_dev[i].bdl);
			}
	}
	if (chip->rb.area) {
		mark_pages_wc(chip, &chip->rb, false);
		snd_dma_free_pages(&chip->rb);
	}
	if (chip->posbuf.area) {
		mark_pages_wc(chip, &chip->posbuf, false);
		snd_dma_free_pages(&chip->posbuf);
	}
	if (chip->pci) {
		pci_release_regions(chip->pci);
		pci_disable_device(chip->pci);
	}
	kfree(chip->azx_dev);
	kfree(chip);
#ifdef CONFIG_SND_HDA_VPR
	if(chip->handle_ref) {
		nvmap_unpin(chip->hda_vpr, chip->handle_ref);
		nvmap_munmap(chip->handle_ref, chip->vaddr);
		nvmap_free(chip->hda_vpr, chip->handle_ref);
	}
#endif

	return 0;
}

static int azx_dev_free(struct snd_device *device)
{
	return azx_free(device->device_data);
}

/*
 * white/black-listing for position_fix
 */
static struct snd_pci_quirk position_fix_list[] __devinitdata = {
	SND_PCI_QUIRK(0x1028, 0x01cc, "Dell D820", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1028, 0x01de, "Dell Precision 390", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x103c, 0x306d, "HP dv3", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x813d, "ASUS P5AD2", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x81b3, "ASUS", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1043, 0x81e7, "ASUS M2V", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x104d, 0x9069, "Sony VPCS11V9E", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x10de, 0xcb89, "Macbook Pro 7,1", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1297, 0x3166, "Shuttle", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1458, 0xa022, "ga-ma770-ud3", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1462, 0x1002, "MSI Wind U115", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1565, 0x8218, "Biostar Microtech", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x1849, 0x0888, "775Dual-VSTA", POS_FIX_LPIB),
	SND_PCI_QUIRK(0x8086, 0x2503, "DG965OT AAD63733-203", POS_FIX_LPIB),
	{}
};

static int __devinit check_position_fix(struct azx *chip, int fix)
{
	const struct snd_pci_quirk *q;

	switch (fix) {
	case POS_FIX_LPIB:
	case POS_FIX_POSBUF:
	case POS_FIX_VIACOMBO:
	case POS_FIX_COMBO:
		return fix;
	}

	if (chip->pci) {
		q = snd_pci_quirk_lookup(chip->pci, position_fix_list);
		if (q) {
			printk(KERN_INFO
			       "hda_intel: position_fix set to %d "
			       "for device %04x:%04x\n",
			       q->value, q->subvendor, q->subdevice);
			return q->value;
		}
	}

	/* Check VIA/ATI HD Audio Controller exist */
	if (chip->driver_caps & AZX_DCAPS_POSFIX_VIA) {
		snd_printd(SFX "Using VIACOMBO position fix\n");
		return POS_FIX_VIACOMBO;
	}
	if (chip->driver_caps & AZX_DCAPS_POSFIX_LPIB) {
		snd_printd(SFX "Using LPIB position fix\n");
		return POS_FIX_LPIB;
	}
	return POS_FIX_AUTO;
}

/*
 * black-lists for probe_mask
 */
static struct snd_pci_quirk probe_mask_list[] __devinitdata = {
	/* Thinkpad often breaks the controller communication when accessing
	 * to the non-working (or non-existing) modem codec slot.
	 */
	SND_PCI_QUIRK(0x1014, 0x05b7, "Thinkpad Z60", 0x01),
	SND_PCI_QUIRK(0x17aa, 0x2010, "Thinkpad X/T/R60", 0x01),
	SND_PCI_QUIRK(0x17aa, 0x20ac, "Thinkpad X/T/R61", 0x01),
	/* broken BIOS */
	SND_PCI_QUIRK(0x1028, 0x20ac, "Dell Studio Desktop", 0x01),
	/* including bogus ALC268 in slot#2 that conflicts with ALC888 */
	SND_PCI_QUIRK(0x17c0, 0x4085, "Medion MD96630", 0x01),
	/* forced codec slots */
	SND_PCI_QUIRK(0x1043, 0x1262, "ASUS W5Fm", 0x103),
	SND_PCI_QUIRK(0x1046, 0x1262, "ASUS W5F", 0x103),
	{}
};

#define AZX_FORCE_CODEC_MASK	0x100

static void __devinit check_probe_mask(struct azx *chip, int dev)
{
	const struct snd_pci_quirk *q;

	chip->codec_probe_mask = probe_mask[dev];
	if (chip->pci && (chip->codec_probe_mask == -1)) {
		q = snd_pci_quirk_lookup(chip->pci, probe_mask_list);
		if (q) {
			printk(KERN_INFO
			       "hda_intel: probe_mask set to 0x%x "
			       "for device %04x:%04x\n",
			       q->value, q->subvendor, q->subdevice);
			chip->codec_probe_mask = q->value;
		}
	}

	/* check forced option */
	if (chip->codec_probe_mask != -1 &&
	    (chip->codec_probe_mask & AZX_FORCE_CODEC_MASK)) {
		chip->codec_mask = chip->codec_probe_mask & 0xff;
		printk(KERN_INFO "hda_intel: codec_mask forced to 0x%x\n",
		       chip->codec_mask);
	}
}

/*
 * white/black-list for enable_msi
 */
static struct snd_pci_quirk msi_black_list[] __devinitdata = {
	SND_PCI_QUIRK(0x1043, 0x81f2, "ASUS", 0), /* Athlon64 X2 + nvidia */
	SND_PCI_QUIRK(0x1043, 0x81f6, "ASUS", 0), /* nvidia */
	SND_PCI_QUIRK(0x1043, 0x822d, "ASUS", 0), /* Athlon64 X2 + nvidia MCP55 */
	SND_PCI_QUIRK(0x1179, 0xfb44, "Toshiba Satellite C870", 0), /* AMD Hudson */
	SND_PCI_QUIRK(0x1849, 0x0888, "ASRock", 0), /* Athlon64 X2 + nvidia */
	SND_PCI_QUIRK(0xa0a0, 0x0575, "Aopen MZ915-M", 0), /* ICH6 */
	{}
};

static void __devinit check_msi(struct azx *chip)
{
	const struct snd_pci_quirk *q;

	/* Disable MSI if chip is not a pci device */
	if (!chip->pci) {
		chip->msi = 0;
		return;
	}

	if (enable_msi >= 0) {
		chip->msi = !!enable_msi;
		return;
	}
	chip->msi = 1;	/* enable MSI as default */
	q = snd_pci_quirk_lookup(chip->pci, msi_black_list);
	if (q) {
		printk(KERN_INFO
		       "hda_intel: msi for device %04x:%04x set to %d\n",
		       q->subvendor, q->subdevice, q->value);
		chip->msi = q->value;
		return;
	}

	/* NVidia chipsets seem to cause troubles with MSI */
	if (chip->driver_caps & AZX_DCAPS_NO_MSI) {
		printk(KERN_INFO "hda_intel: Disabling MSI\n");
		chip->msi = 0;
	}
}

/* check the snoop mode availability */
static void __devinit azx_check_snoop_available(struct azx *chip)
{
	bool snoop = chip->snoop;

	switch (chip->driver_type) {
	case AZX_DRIVER_VIA:
		/* force to non-snoop mode for a new VIA controller
		 * when BIOS is set
		 */
		if (snoop) {
			u8 val;
			pci_read_config_byte(chip->pci, 0x42, &val);
			if (!(val & 0x80) && chip->pci->revision == 0x30)
				snoop = false;
		}
		break;
	case AZX_DRIVER_ATIHDMI_NS:
		/* new ATI HDMI requires non-snoop */
		snoop = false;
		break;
	}

	if (snoop != chip->snoop) {
		snd_printk(KERN_INFO SFX "Force to %s mode\n",
			   snoop ? "snoop" : "non-snoop");
		chip->snoop = snoop;
	}
}

#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
static const char *tegra_clk_names[] __devinitdata = {
	"hda",
	"hda2codec",
	"hda2hdmi",
};
static struct clk *tegra_clks[ARRAY_SIZE(tegra_clk_names)];
#endif

/*
 * constructor
 */
static int __devinit azx_create(struct snd_card *card, struct pci_dev *pci,
				struct platform_device *pdev,
				int dev, unsigned int driver_caps,
				struct azx **rchip)
{
	struct azx *chip;
	int i, err = 0;
	unsigned short gcap;
	static struct snd_device_ops ops = {
		.dev_free = azx_dev_free,
	};

	*rchip = NULL;

	if (pci) {
		err = pci_enable_device(pci);
		if (err < 0)
			return err;
	}

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		snd_printk(KERN_ERR SFX "cannot allocate chip\n");
		if (pci)
			pci_disable_device(pci);
		return -ENOMEM;
	}

	spin_lock_init(&chip->reg_lock);
	mutex_init(&chip->open_mutex);
	chip->card = card;
	chip->pci = pci;
	chip->pdev = pdev;
	chip->dev = pci ? snd_dma_pci_data(pci) : &pdev->dev;
	chip->irq_id = pci ? pci->irq : platform_get_irq(pdev, 0);
	chip->irq = -1;
	chip->driver_caps = driver_caps;
	chip->driver_type = driver_caps & 0xff;
	check_msi(chip);
	chip->dev_index = dev;
	INIT_WORK(&chip->irq_pending_work, azx_irq_pending_work);
	INIT_LIST_HEAD(&chip->pcm_list);

	chip->position_fix[0] = chip->position_fix[1] =
		check_position_fix(chip, position_fix[dev]);
	/* combo mode uses LPIB for playback */
	if (chip->position_fix[0] == POS_FIX_COMBO) {
		chip->position_fix[0] = POS_FIX_LPIB;
		chip->position_fix[1] = POS_FIX_AUTO;
	}

	check_probe_mask(chip, dev);

	chip->single_cmd = single_cmd;
	chip->snoop = hda_snoop;
	azx_check_snoop_available(chip);

	if (bdl_pos_adj[dev] < 0) {
		switch (chip->driver_type) {
		case AZX_DRIVER_ICH:
		case AZX_DRIVER_PCH:
			bdl_pos_adj[dev] = 1;
			break;
		default:
			bdl_pos_adj[dev] = 32;
			break;
		}
	}

#if BITS_PER_LONG != 64
	/* Fix up base address on ULI M5461 */
	if (chip->driver_type == AZX_DRIVER_ULI) {
		u16 tmp3;
		pci_read_config_word(pci, 0x40, &tmp3);
		pci_write_config_word(pci, 0x40, tmp3 | 0x10);
		pci_write_config_dword(pci, PCI_BASE_ADDRESS_1, 0);
	}
#endif

	if (chip->pci) {
		err = pci_request_regions(pci, "ICH HD audio");
		if (err < 0) {
			kfree(chip);
			pci_disable_device(pci);
			return err;
		}

		chip->addr = pci_resource_start(pci, 0);
		chip->remap_addr = pci_ioremap_bar(pci, 0);
		if (chip->remap_addr == NULL) {
			snd_printk(KERN_ERR SFX "ioremap error\n");
			err = -ENXIO;
			goto errout;
		}

		if (chip->msi)
			if (pci_enable_msi(pci) < 0)
				chip->msi = 0;
	}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	if (chip->pdev) {
		struct resource *res, *region;

		/* Do platform specific initialization */
		switch (chip->driver_type) {
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
		case AZX_DRIVER_NVIDIA_TEGRA:
			chip->platform_clk_count = ARRAY_SIZE(tegra_clk_names);
			for (i = 0; i < chip->platform_clk_count; i++) {
				tegra_clks[i] = clk_get(&pdev->dev,
							tegra_clk_names[i]);
				if (IS_ERR_OR_NULL(tegra_clks[i])) {
					err = PTR_ERR(tegra_clks[i]);
					goto errout;
				}
			}
			chip->platform_clks = tegra_clks;
			break;
#endif
		default:
			break;
		}

		azx_platform_enable_clocks(chip);

		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (res == NULL) {
			err = EINVAL;
			goto errout;
		}

		region = devm_request_mem_region(chip->dev, res->start,
						 resource_size(res),
						 pdev->name);
		if (!region) {
			snd_printk(KERN_ERR SFX "Mem region already claimed\n");
			err = -EINVAL;
			goto errout;
		}

		chip->addr = res->start;
		chip->remap_addr = devm_ioremap(chip->dev,
						res->start,
						resource_size(res));
		if (chip->remap_addr == NULL) {
			snd_printk(KERN_ERR SFX "ioremap error\n");
			err = -ENXIO;
			goto errout;
		}

#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
		if (chip->driver_type == AZX_DRIVER_NVIDIA_TEGRA) {
			chip->remap_config_addr = chip->remap_addr;
			chip->remap_addr += NVIDIA_TEGRA_HDA_BAR0_OFFSET;
			chip->addr += NVIDIA_TEGRA_HDA_BAR0_OFFSET;
		}
#endif

		azx_init_platform(chip);
	}
#endif /* CONFIG_SND_HDA_PLATFORM_DRIVER */

	if (azx_acquire_irq(chip, 0) < 0) {
		err = -EBUSY;
		goto errout;
	}

	if (chip->pci)
		pci_set_master(pci);

	synchronize_irq(chip->irq);

	gcap = azx_readw(chip, GCAP);
	snd_printdd(SFX "chipset global capabilities = 0x%x\n", gcap);

	/* disable SB600 64bit support for safety */
	if (chip->pci && chip->pci->vendor == PCI_VENDOR_ID_ATI) {
		struct pci_dev *p_smbus;
		p_smbus = pci_get_device(PCI_VENDOR_ID_ATI,
					 PCI_DEVICE_ID_ATI_SBX00_SMBUS,
					 NULL);
		if (p_smbus) {
			if (p_smbus->revision < 0x30)
				gcap &= ~ICH6_GCAP_64OK;
			pci_dev_put(p_smbus);
		}
	}

	/* disable 64bit DMA address on some devices */
	if (chip->driver_caps & AZX_DCAPS_NO_64BIT) {
		snd_printd(SFX "Disabling 64bit DMA\n");
		gcap &= ~ICH6_GCAP_64OK;
	}

	/* disable buffer size rounding to 128-byte multiples if supported */
	if (align_buffer_size >= 0)
		chip->align_buffer_size = !!align_buffer_size;
	else {
		if (chip->driver_caps & AZX_DCAPS_BUFSIZE)
			chip->align_buffer_size = 0;
		else if (chip->driver_caps & AZX_DCAPS_ALIGN_BUFSIZE)
			chip->align_buffer_size = 1;
		else
			chip->align_buffer_size = 1;
	}

	if (chip->pci) {
		/* allow 64bit DMA address if supported by H/W */
		if ((gcap & ICH6_GCAP_64OK) && !pci_set_dma_mask(pci, DMA_BIT_MASK(64)))
			pci_set_consistent_dma_mask(pci, DMA_BIT_MASK(64));
		else {
			pci_set_dma_mask(pci, DMA_BIT_MASK(32));
			pci_set_consistent_dma_mask(pci, DMA_BIT_MASK(32));
		}
	}

	/* read number of streams from GCAP register instead of using
	 * hardcoded value
	 */
	chip->capture_streams = (gcap >> 8) & 0x0f;
	chip->playback_streams = (gcap >> 12) & 0x0f;
	if (!chip->playback_streams && !chip->capture_streams) {
		/* gcap didn't give any info, switching to old method */

		switch (chip->driver_type) {
		case AZX_DRIVER_ULI:
			chip->playback_streams = ULI_NUM_PLAYBACK;
			chip->capture_streams = ULI_NUM_CAPTURE;
			break;
		case AZX_DRIVER_ATIHDMI:
		case AZX_DRIVER_ATIHDMI_NS:
			chip->playback_streams = ATIHDMI_NUM_PLAYBACK;
			chip->capture_streams = ATIHDMI_NUM_CAPTURE;
			break;
		case AZX_DRIVER_GENERIC:
		default:
			chip->playback_streams = ICH6_NUM_PLAYBACK;
			chip->capture_streams = ICH6_NUM_CAPTURE;
			break;
		}
	}
	chip->capture_index_offset = 0;
	chip->playback_index_offset = chip->capture_streams;
	chip->num_streams = chip->playback_streams + chip->capture_streams;
	chip->azx_dev = kcalloc(chip->num_streams, sizeof(*chip->azx_dev),
				GFP_KERNEL);
	if (!chip->azx_dev) {
		snd_printk(KERN_ERR SFX "cannot malloc azx_dev\n");
		goto errout;
	}

	for (i = 0; i < chip->num_streams; i++) {
		/* allocate memory for the BDL for each stream */
		err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV,
					  chip->dev,
					  BDL_SIZE, &chip->azx_dev[i].bdl);
		if (err < 0) {
			snd_printk(KERN_ERR SFX "cannot allocate BDL\n");
			goto errout;
		}
		mark_pages_wc(chip, &chip->azx_dev[i].bdl, true);
	}
	/* allocate memory for the position buffer */
	err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV,
				  chip->dev,
				  chip->num_streams * 8, &chip->posbuf);
	if (err < 0) {
		snd_printk(KERN_ERR SFX "cannot allocate posbuf\n");
		goto errout;
	}
	mark_pages_wc(chip, &chip->posbuf, true);
	/* allocate CORB/RIRB */
	err = azx_alloc_cmd_io(chip);
	if (err < 0)
		goto errout;

	/* initialize streams */
	azx_init_stream(chip);

	/* initialize chip */
	if (chip->pci)
		azx_init_pci(chip);
	azx_init_chip(chip, (probe_only[dev] & 2) == 0);

	/* codec detection */
	if (!chip->codec_mask) {
		snd_printk(KERN_ERR SFX "no codecs found!\n");
		err = -ENODEV;
		goto errout;
	}

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops);
	if (err <0) {
		snd_printk(KERN_ERR SFX "Error creating device [card]!\n");
		goto errout;
	}

	strcpy(card->driver, "HDA-Intel");
	strlcpy(card->shortname, driver_short_names[chip->driver_type],
		sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s at 0x%lx irq %i",
		 card->shortname, chip->addr, chip->irq);

	*rchip = chip;
	return 0;

 errout:
	azx_free(chip);
	return err;
}

static void power_down_all_codecs(struct azx *chip)
{
#ifdef CONFIG_SND_HDA_POWER_SAVE
	/* The codecs were powered up in snd_hda_codec_new().
	 * Now all initialization done, so turn them down if possible
	 */
	struct hda_codec *codec;
	list_for_each_entry(codec, &chip->bus->codec_list, list) {
		snd_hda_power_down(codec);
	}
#endif
}

static int __devinit azx_probe(struct pci_dev *pci,
			       struct platform_device *pdev,
			       int driver_data)
{
	static int dev;
	struct snd_card *card;
	struct azx *chip;
	struct device *azx_dev = pci ? &pci->dev : &pdev->dev;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	err = snd_card_create(index[dev], id[dev], THIS_MODULE, 0, &card);
	if (err < 0) {
		snd_printk(KERN_ERR SFX "Error creating card!\n");
		return err;
	}

	/* set this here since it's referred in snd_hda_load_patch() */
	snd_card_set_dev(card, azx_dev);

	err = azx_create(card, pci, pdev, dev, driver_data, &chip);
	if (err < 0)
		goto out_free;
	card->private_data = chip;

#ifdef CONFIG_SND_HDA_INPUT_BEEP
	chip->beep_mode = beep_mode[dev];
#endif

	/* create codec instances */
	err = azx_codec_create(chip, model[dev]);
	if (err < 0)
		goto out_free;
#ifdef CONFIG_SND_HDA_PATCH_LOADER
	if (patch[dev] && *patch[dev]) {
		snd_printk(KERN_ERR SFX "Applying patch firmware '%s'\n",
			   patch[dev]);
		err = snd_hda_load_patch(chip->bus, patch[dev]);
		if (err < 0)
			goto out_free;
	}
#endif
	if ((probe_only[dev] & 1) == 0) {
		err = azx_codec_configure(chip);
		if (err < 0)
			goto out_free;
	}

	/* create PCM streams */
	err = snd_hda_build_pcms(chip->bus);
	if (err < 0)
		goto out_free;

	/* create mixer controls */
	err = azx_mixer_create(chip);
	if (err < 0)
		goto out_free;

	err = snd_card_register(card);
	if (err < 0)
		goto out_free;

	if (pci)
		pci_set_drvdata(pci, card);
	else
		dev_set_drvdata(&pdev->dev, card);

	chip->running = 1;
	power_down_all_codecs(chip);
	azx_notifier_register(chip);

	dev++;
	return err;
out_free:
	snd_card_free(card);
	return err;
}

static int __devinit azx_probe_pci(struct pci_dev *pci,
				   const struct pci_device_id *pci_id)
{
	return azx_probe(pci, NULL, pci_id->driver_data);
}

static void __devexit azx_remove_pci(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

/* PCI IDs */
static DEFINE_PCI_DEVICE_TABLE(azx_pci_ids) = {
	/* CPT */
	{ PCI_DEVICE(0x8086, 0x1c20),
	  .driver_data = AZX_DRIVER_PCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE },
	/* PBG */
	{ PCI_DEVICE(0x8086, 0x1d20),
	  .driver_data = AZX_DRIVER_PCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE},
	/* Panther Point */
	{ PCI_DEVICE(0x8086, 0x1e20),
	  .driver_data = AZX_DRIVER_PCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE},
	/* Lynx Point */
	{ PCI_DEVICE(0x8086, 0x8c20),
	  .driver_data = AZX_DRIVER_PCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE},
	/* SCH */
	{ PCI_DEVICE(0x8086, 0x811b),
	  .driver_data = AZX_DRIVER_SCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE | AZX_DCAPS_POSFIX_LPIB }, /* Poulsbo */
	{ PCI_DEVICE(0x8086, 0x080a),
	  .driver_data = AZX_DRIVER_SCH | AZX_DCAPS_SCH_SNOOP |
	  AZX_DCAPS_BUFSIZE | AZX_DCAPS_POSFIX_LPIB }, /* Oaktrail */
	/* ICH */
	{ PCI_DEVICE(0x8086, 0x2668),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH6 */
	{ PCI_DEVICE(0x8086, 0x27d8),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH7 */
	{ PCI_DEVICE(0x8086, 0x269a),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ESB2 */
	{ PCI_DEVICE(0x8086, 0x284b),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH8 */
	{ PCI_DEVICE(0x8086, 0x293e),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH9 */
	{ PCI_DEVICE(0x8086, 0x293f),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH9 */
	{ PCI_DEVICE(0x8086, 0x3a3e),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH10 */
	{ PCI_DEVICE(0x8086, 0x3a6e),
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_OLD_SSYNC |
	  AZX_DCAPS_BUFSIZE },  /* ICH10 */
	/* Generic Intel */
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_ICH | AZX_DCAPS_BUFSIZE },
	/* ATI SB 450/600/700/800/900 */
	{ PCI_DEVICE(0x1002, 0x437b),
	  .driver_data = AZX_DRIVER_ATI | AZX_DCAPS_PRESET_ATI_SB },
	{ PCI_DEVICE(0x1002, 0x4383),
	  .driver_data = AZX_DRIVER_ATI | AZX_DCAPS_PRESET_ATI_SB },
	/* AMD Hudson */
	{ PCI_DEVICE(0x1022, 0x780d),
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_SB },
	/* ATI HDMI */
	{ PCI_DEVICE(0x1002, 0x793b),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0x7919),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0x960f),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0x970f),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa00),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa08),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa10),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa18),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa20),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa28),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa30),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa38),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa40),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaa48),
	  .driver_data = AZX_DRIVER_ATIHDMI | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0x9902),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaaa0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaaa8),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(0x1002, 0xaab0),
	  .driver_data = AZX_DRIVER_ATIHDMI_NS | AZX_DCAPS_PRESET_ATI_HDMI },
	/* VIA VT8251/VT8237A */
	{ PCI_DEVICE(0x1106, 0x3288),
	  .driver_data = AZX_DRIVER_VIA | AZX_DCAPS_POSFIX_VIA },
	/* SIS966 */
	{ PCI_DEVICE(0x1039, 0x7502), .driver_data = AZX_DRIVER_SIS },
	/* ULI M5461 */
	{ PCI_DEVICE(0x10b9, 0x5461), .driver_data = AZX_DRIVER_ULI },
	/* NVIDIA MCP */
	{ PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_NVIDIA | AZX_DCAPS_PRESET_NVIDIA },
	/* Teradici */
	{ PCI_DEVICE(0x6549, 0x1200),
	  .driver_data = AZX_DRIVER_TERA | AZX_DCAPS_NO_64BIT },
	/* Creative X-Fi (CA0110-IBG) */
#if !defined(CONFIG_SND_CTXFI) && !defined(CONFIG_SND_CTXFI_MODULE)
	/* the following entry conflicts with snd-ctxfi driver,
	 * as ctxfi driver mutates from HD-audio to native mode with
	 * a special command sequence.
	 */
	{ PCI_DEVICE(PCI_VENDOR_ID_CREATIVE, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_CTX | AZX_DCAPS_CTX_WORKAROUND |
	  AZX_DCAPS_NO_64BIT |
	  AZX_DCAPS_RIRB_PRE_DELAY | AZX_DCAPS_POSFIX_LPIB },
#else
	/* this entry seems still valid -- i.e. without emu20kx chip */
	{ PCI_DEVICE(0x1102, 0x0009),
	  .driver_data = AZX_DRIVER_CTX | AZX_DCAPS_CTX_WORKAROUND |
	  AZX_DCAPS_NO_64BIT |
	  AZX_DCAPS_RIRB_PRE_DELAY | AZX_DCAPS_POSFIX_LPIB },
#endif
	/* Vortex86MX */
	{ PCI_DEVICE(0x17f3, 0x3010), .driver_data = AZX_DRIVER_GENERIC },
	/* VMware HDAudio */
	{ PCI_DEVICE(0x15ad, 0x1977), .driver_data = AZX_DRIVER_GENERIC },
	/* AMD/ATI Generic, PCI class code and Vendor ID for HD Audio */
	{ PCI_DEVICE(PCI_VENDOR_ID_ATI, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_HDMI },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_ANY_ID),
	  .class = PCI_CLASS_MULTIMEDIA_HD_AUDIO << 8,
	  .class_mask = 0xffffff,
	  .driver_data = AZX_DRIVER_GENERIC | AZX_DCAPS_PRESET_ATI_HDMI },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, azx_pci_ids);

/* pci_driver definition */
static struct pci_driver driver = {
	.name = KBUILD_MODNAME,
	.id_table = azx_pci_ids,
	.probe = azx_probe_pci,
	.remove = __devexit_p(azx_remove_pci),
#ifdef CONFIG_PM
	.suspend = azx_suspend_pci,
	.resume = azx_resume_pci,
#endif
};

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
static int __devinit azx_probe_platform(struct platform_device *pdev)
{
	const struct platform_device_id *pdev_id = platform_get_device_id(pdev);

	return azx_probe(NULL, pdev, pdev_id->driver_data);
}

static int __devexit azx_remove_platform(struct platform_device *pdev)
{
	return snd_card_free(dev_get_drvdata(&pdev->dev));
}

static const struct platform_device_id azx_platform_ids[] = {
#ifdef CONFIG_SND_HDA_PLATFORM_NVIDIA_TEGRA
	{ "tegra30-hda",
	  .driver_data = AZX_DRIVER_NVIDIA_TEGRA | AZX_DCAPS_RIRB_DELAY },
#endif
	{ },
};
MODULE_DEVICE_TABLE(platform, azx_platform_ids);

/* platform_driver definition */
static struct platform_driver hda_platform_driver = {
	.driver = {
		.name = "hda-platform"
	},
	.probe = azx_probe_platform,
	.remove = __devexit_p(azx_remove_platform),
	.id_table = azx_platform_ids,
#ifdef CONFIG_PM
	.suspend = azx_suspend_platform,
	.resume = azx_resume_platform,
#endif
};
#endif /* CONFIG_SND_HDA_PLATFORM_DRIVER */

static int __init alsa_card_azx_init(void)
{
	int err = 0;

	err = pci_register_driver(&driver);
	if (err < 0) {
		snd_printk(KERN_ERR SFX "Failed to register pci driver\n");
		return err;
	}

#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	err = platform_driver_register(&hda_platform_driver);
	if (err < 0) {
		snd_printk(KERN_ERR SFX "Failed to register platform driver\n");
		pci_unregister_driver(&driver);
		return err;
	}
#endif

	return 0;
}

static void __exit alsa_card_azx_exit(void)
{
#ifdef CONFIG_SND_HDA_PLATFORM_DRIVER
	platform_driver_unregister(&hda_platform_driver);
#endif

	pci_unregister_driver(&driver);
}

module_init(alsa_card_azx_init)
module_exit(alsa_card_azx_exit)
