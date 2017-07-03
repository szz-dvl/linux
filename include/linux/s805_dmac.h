#include <mach/am_regs.h>
#include <../drivers/dma/virt-dma.h>

typedef enum inline_types {
	INLINE_NORMAL,
	INLINE_TDES,
	INLINE_DIVX,
	INLINE_CRC,
	INLINE_AES
} s805_dma_tr_type;

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
	s805_dtable * next;

	/* Boolean to mark transactions as terminated. */
    bool terminated;
	
};

static inline struct s805_desc *to_s805_dma_desc(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct s805_desc, vd.tx);
}
