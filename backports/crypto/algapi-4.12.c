#include <linux/crypto.h>

unsigned int crypto_alg_extsize(struct crypto_alg *alg)
{
	return alg->cra_ctxsize +
	       (alg->cra_alignmask & ~(crypto_tfm_ctx_alignment() - 1));
}
EXPORT_SYMBOL_GPL(crypto_alg_extsize);
