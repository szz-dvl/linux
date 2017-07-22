#include <linux/kernel.h>
#include <linux/dmapool.h>

#ifndef __S805_DMAC
#include <mach/am_regs.h>
#include <linux/dmaengine.h>
#include <../drivers/dma/virt-dma.h>

#define WR(data, addr)  *(volatile unsigned long *)(addr)=data
#define RD(addr)        *(volatile unsigned long *)(addr)

/* S805 Datasheet p.57 */
struct s805_table_desc 
{
	u32 control;        /* entry 0 */
	u32 src;            /* entry 1 */
	u32 dst;            /* entry 2 */
	u32 count;          /* entry 3 */
	
	/* 2D Move */
	u16 src_burst;      /* entry 4 [15:0]  */
	u16 src_skip;       /* entry 4 [31:16] */
	
	u16 dst_burst;      /* entry 5 [15:0]  */
	u16 dst_skip;       /* entry 5 [31:16] */
	
	
	/* Crypto engine */
	u32 crypto;         /* entry 6 */
	
} __attribute__ ((aligned (32)));

typedef struct s805_table_desc_entry {

	struct list_head elem;
	struct s805_table_desc * table;
	dma_addr_t paddr;

} s805_dtable;

struct s805_desc {
	
	struct s805_chan *c;
	struct virt_dma_desc vd;
	struct list_head elem;
	
	/* List of table descriptors holding the information of the trasaction */
	struct list_head desc_list;
	
	/* Descriptors pending of process */
	unsigned int frames;

	/* Struct to store the information for memset source value */
	struct memset_info * memset;

	/* For transactions with more than S805_DMA_MAX_DESC data chunks. */
	s805_dtable * next_chunk;

	/* For cyclic transfers */
	struct s805_desc * next;
	struct s805_desc * root;	
};

#endif

#define S805_DTBL_INLINE_TYPE(type)      ((type & 0x7) << 22)
#define S805_DTBL_PRE_ENDIAN(type)       ((type & 0x7) << 27)

#define S805_DMA_MAX_DESC                127
#define S805_DTBL_IRQ                    BIT(21)
#define S805_DTBL_SRC_HOLD               BIT(26) 
#define S805_DTBL_DST_HOLD               BIT(25)

/* S805 Datasheet p.58 */
typedef enum inline_types {
	INLINE_NORMAL,
	INLINE_TDES,
	INLINE_DIVX,
	INLINE_CRC,
	INLINE_AES
} s805_dma_tr_type;

typedef enum desc_types {
    BLKMV_DESC,
    AES_DESC,
    TDES_DESC,
	CRC_DESC,
	DIVX_DESC
} s805_desc_type;

/* S805 Datasheet p.58 */
typedef enum endian_types {
	ENDIAN_NO_CHANGE,
	ENDIAN_SWAP_BYTES,
	ENDIAN_SWAP_WORDS,
	ENDIAN_REVERSE,
	ENDIAN_TYPE_4,
	ENDIAN_TYPE_5,
	ENDIAN_TYPE_6,
	ENDIAN_TYPE_7
} s805_dma_endian_type;

typedef enum aes_key_type {
	AES_KEY_TYPE_128,
	AES_KEY_TYPE_192,
	AES_KEY_TYPE_256,
	AES_KEY_TYPE_RESERVED,
} s805_aes_key_type;

typedef enum aes_mode {
	AES_MODE_ECB,
    AES_MODE_CBC,
	AES_MODE_CTR,
	AES_MODE_RESERVED,
} s805_aes_mode;

typedef enum aes_dir {
	AES_DIR_DECRYPT,
    AES_DIR_ENCRYPT
} s805_aes_dir;

typedef enum tdes_mode {
	TDES_MODE_ECB,
    TDES_MODE_CBC
} s805_tdes_mode;

typedef enum s805_dma_status {
	S805_DMA_SUCCESS,
	S805_DMA_IN_PROGRESS,
	S805_DMA_PAUSED,
	S805_DMA_ERROR,
	S805_DMA_TERMINATED
} s805_status;

typedef struct s805_chan {
	
	struct virt_dma_chan vc;

	/* Channel configuration, needed for slave_sg and cyclic transfers. */
	struct dma_slave_config cfg;

	/* Status of the channel either DMA_PAUSE ,DMA_IN_PROGRESS, DMA_SUCCESS or DMA_TERMINATED if terminated channel. */
    s805_status status;        	

	/* DMA pool. */
	struct dma_pool *pool;

	/* Pending transactions for the channel */
	int pending;
	
} s805_chan;

typedef struct aes_init_descriptor {

	s805_aes_key_type type;
	s805_aes_mode mode;
	s805_aes_dir dir;


} s805_init_aes_desc;

typedef struct init_descriptor {

	uint frames;
	s805_desc_type type;
	s805_init_aes_desc aes_nfo;
	s805_tdes_mode tdes_mode;
	struct s805_desc * d;
	
} s805_init_desc;

static inline struct s805_chan *to_s805_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct s805_chan, vc.chan);
}

struct dma_async_tx_descriptor * s805_scatterwalk (struct scatterlist * src_sg,
												   struct scatterlist * dst_sg,
												   s805_init_desc * init_nfo,
												   struct dma_async_tx_descriptor * tx_desc,
												   bool last);

bool s805_close_desc (struct dma_async_tx_descriptor * tx_desc);     /* CRC  */

s805_dtable * sg_aes_move_along (s805_dtable * cursor, s805_init_desc * init_nfo);
s805_dtable * sg_tdes_move_along (s805_dtable * cursor, s805_init_desc * init_nfo);
s805_dtable * sg_crc_move_along (s805_dtable * cursor, s805_init_desc * init_nfo);
s805_dtable * sg_divx_move_along (s805_dtable * cursor, s805_init_desc * init_nfo);
