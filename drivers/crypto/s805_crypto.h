#include <linux/crypto.h>
#include <crypto/algapi.h>

static inline struct ablkcipher_request * to_ablkcipher_request(void *rctx)
{
	return container_of(rctx, struct ablkcipher_request, __ctx);
}
