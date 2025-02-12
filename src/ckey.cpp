#include "compat.h"

#include <cstring>

#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include "crypto/common/common.h"

#include "util.h"
#include "chash256.h"
#include "uint/uint256.h"
#include "cpubkey.h"
#include "hash.h"
#include "crypter.h"
#include "allocators.h"

#include "ckey.h"

static secp256k1_context* secp256k1_context_sign = NULL;

/** These functions are taken from the libsecp256k1 distribution and are very ugly. */
static int ec_privkey_import_der(const secp256k1_context* ctx, unsigned char *out32,
		const unsigned char *privkey, size_t privkeylen)
{
	const unsigned char *end = privkey + privkeylen;
	int lenb = 0;
	int len = 0;

	memset(out32, 0, 32);

	/* sequence header */
	if (end < privkey + 1 || *privkey != 0x30)
	{
		return 0;
	}

	privkey++;

	/* sequence length constructor */
	if (end < privkey + 1 || !(*privkey & 0x80))
	{
		return 0;
	}

	lenb = *privkey & ~0x80; privkey++;

	if (lenb < 1 || lenb > 2)
	{
		return 0;
	}

	if (end < privkey + lenb)
	{
		return 0;
	}

	/* sequence length */
	len = privkey[lenb-1] | (lenb > 1 ? privkey[lenb-2] << 8 : 0);
	privkey += lenb;

	if (end < privkey+len)
	{
		return 0;
	}

	/* sequence element 0: version number (=1) */
	if (end < privkey+3 || privkey[0] != 0x02 || privkey[1] != 0x01 || privkey[2] != 0x01)
	{
	   return 0;
	}

	privkey += 3;

	/* sequence element 1: octet string, up to 32 bytes */
	if (end < privkey+2 || privkey[0] != 0x04 || privkey[1] > 0x20 || end < privkey+2+privkey[1])
	{
		return 0;
	}

	memcpy(out32 + 32 - privkey[1], privkey + 2, privkey[1]);

	if (!secp256k1_ec_seckey_verify(ctx, out32))
	{
		memset(out32, 0, 32);
		
		return 0;
	}

	return 1;
}

static int ec_privkey_export_der(const secp256k1_context *ctx, unsigned char *privkey, size_t *privkeylen,
		const unsigned char *key32, int compressed)
{
	secp256k1_pubkey pubkey;
	size_t pubkeylen = 0;

	if (!secp256k1_ec_pubkey_create(ctx, &pubkey, key32))
	{
		*privkeylen = 0;
		
		return 0;
	}

	if (compressed)
	{
		static const unsigned char begin[] = {
			0x30,0x81,0xD3,0x02,0x01,0x01,0x04,0x20
		};
		static const unsigned char middle[] = {
			0xA0,0x81,0x85,0x30,0x81,0x82,0x02,0x01,0x01,0x30,0x2C,0x06,0x07,0x2A,0x86,0x48,
			0xCE,0x3D,0x01,0x01,0x02,0x21,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFE,0xFF,0xFF,0xFC,0x2F,0x30,0x06,0x04,0x01,0x00,0x04,0x01,0x07,0x04,
			0x21,0x02,0x79,0xBE,0x66,0x7E,0xF9,0xDC,0xBB,0xAC,0x55,0xA0,0x62,0x95,0xCE,0x87,
			0x0B,0x07,0x02,0x9B,0xFC,0xDB,0x2D,0xCE,0x28,0xD9,0x59,0xF2,0x81,0x5B,0x16,0xF8,
			0x17,0x98,0x02,0x21,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFF,0xFF,0xFE,0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,0xBF,0xD2,0x5E,
			0x8C,0xD0,0x36,0x41,0x41,0x02,0x01,0x01,0xA1,0x24,0x03,0x22,0x00
		};
		unsigned char *ptr = privkey;
		
		memcpy(ptr, begin, sizeof(begin)); ptr += sizeof(begin);
		memcpy(ptr, key32, 32); ptr += 32;
		memcpy(ptr, middle, sizeof(middle)); ptr += sizeof(middle);
		
		pubkeylen = 33;
		
		secp256k1_ec_pubkey_serialize(ctx, ptr, &pubkeylen, &pubkey, SECP256K1_EC_COMPRESSED);
		
		ptr += pubkeylen;
		
		*privkeylen = ptr - privkey;
	}
	else
	{
		static const unsigned char begin[] = {
			0x30,0x82,0x01,0x13,0x02,0x01,0x01,0x04,0x20
		};
		static const unsigned char middle[] = {
			0xA0,0x81,0xA5,0x30,0x81,0xA2,0x02,0x01,0x01,0x30,0x2C,0x06,0x07,0x2A,0x86,0x48,
			0xCE,0x3D,0x01,0x01,0x02,0x21,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFE,0xFF,0xFF,0xFC,0x2F,0x30,0x06,0x04,0x01,0x00,0x04,0x01,0x07,0x04,
			0x41,0x04,0x79,0xBE,0x66,0x7E,0xF9,0xDC,0xBB,0xAC,0x55,0xA0,0x62,0x95,0xCE,0x87,
			0x0B,0x07,0x02,0x9B,0xFC,0xDB,0x2D,0xCE,0x28,0xD9,0x59,0xF2,0x81,0x5B,0x16,0xF8,
			0x17,0x98,0x48,0x3A,0xDA,0x77,0x26,0xA3,0xC4,0x65,0x5D,0xA4,0xFB,0xFC,0x0E,0x11,
			0x08,0xA8,0xFD,0x17,0xB4,0x48,0xA6,0x85,0x54,0x19,0x9C,0x47,0xD0,0x8F,0xFB,0x10,
			0xD4,0xB8,0x02,0x21,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
			0xFF,0xFF,0xFF,0xFF,0xFE,0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,0xBF,0xD2,0x5E,
			0x8C,0xD0,0x36,0x41,0x41,0x02,0x01,0x01,0xA1,0x44,0x03,0x42,0x00
		};
		unsigned char *ptr = privkey;
		
		memcpy(ptr, begin, sizeof(begin)); ptr += sizeof(begin);
		memcpy(ptr, key32, 32); ptr += 32;
		memcpy(ptr, middle, sizeof(middle)); ptr += sizeof(middle);
		
		pubkeylen = 65;
		
		secp256k1_ec_pubkey_serialize(ctx, ptr, &pubkeylen, &pubkey, SECP256K1_EC_UNCOMPRESSED);
		
		ptr += pubkeylen;
		
		*privkeylen = ptr - privkey;
	}

	return 1;
}

bool operator==(const CKey &a, const CKey &b)
{
	return (
		a.fCompressed == b.fCompressed &&
		a.size() == b.size() &&
		memcmp(&a.vch[0], &b.vch[0], a.size()) == 0
	);
}

// Construct an invalid private key.
CKey::CKey() : fValid(false)
{
	LockObject(vch);
}

// Copy constructor. This is necessary because of memlocking.
CKey::CKey(const CKey &secret) : fValid(secret.fValid), fCompressed(secret.fCompressed)
{
	LockObject(vch);

	memcpy(vch, secret.vch, sizeof(vch));
}

// Destructor (again necessary because of memlocking).
CKey::~CKey()
{
	UnlockObject(vch);
}

// Initialize using begin and end iterators to byte data.
template<typename T>
void CKey::Set(const T pbegin, const T pend, bool fCompressedIn)
{
	if (pend - pbegin != 32)
	{
		fValid = false;
		
		return;
	}

	if (Check(&pbegin[0]))
	{
		memcpy(vch, (unsigned char*)&pbegin[0], 32);
		
		fValid = true;
		fCompressed = fCompressedIn;
	}
	else
	{
		fValid = false;
	}
}

template void CKey::Set<unsigned char*>(unsigned char*, unsigned char*, bool); 
template void CKey::Set<unsigned char const*>(unsigned char const*, unsigned char const*, bool);
template void CKey::Set<CKeyingMaterial::iterator>(CKeyingMaterial::iterator, CKeyingMaterial::iterator, bool);

// Simple read-only vector-like interface.
unsigned int CKey::size() const
{
	return (fValid ? 32 : 0);
}

const unsigned char* CKey::begin() const
{
	return vch;
}

const unsigned char* CKey::end() const
{
	return vch + size();
}

// Check whether this private key is valid.
bool CKey::IsValid() const
{
	return fValid;
}

// Check whether the public key corresponding to this private key is (to be) compressed.
bool CKey::IsCompressed() const
{
	return fCompressed;
}

bool CKey::SetPrivKey(const CPrivKey &privkey, bool fCompressedIn)
{
	if (!ec_privkey_import_der(secp256k1_context_sign, (unsigned char*)begin(), &privkey[0], privkey.size()))
	{
		return false;
	}

	fCompressed = fCompressedIn;
	fValid = true;

	return true;
}

void CKey::MakeNewKey(bool fCompressedIn)
{
	RandAddSeedPerfmon();

	do
	{
		GetRandBytes(vch, sizeof(vch));
	} while (!Check(vch));

	fValid = true;
	fCompressed = fCompressedIn;
}

CPrivKey CKey::GetPrivKey() const
{
	assert(fValid);

	CPrivKey privkey;
	int ret;
	size_t privkeylen;

	privkey.resize(279);
	privkeylen = 279;
	ret = ec_privkey_export_der(
		secp256k1_context_sign,
		(unsigned char*)&privkey[0],
		&privkeylen,
		begin(),
		fCompressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED
	);

	assert(ret);

	privkey.resize(privkeylen);

	return privkey;
}

CPubKey CKey::GetPubKey() const
{
	assert(fValid);

	secp256k1_pubkey pubkey;
	size_t clen = 65;
	CPubKey result;
	int ret = secp256k1_ec_pubkey_create(secp256k1_context_sign, &pubkey, begin());

	assert(ret);

	secp256k1_ec_pubkey_serialize(
		secp256k1_context_sign,
		(unsigned char*)result.begin(),
		&clen,
		&pubkey,
		fCompressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED
	);

	assert(result.size() == clen);
	assert(result.IsValid());

	return result;
}

bool CKey::Sign(const uint256 &hash, std::vector<unsigned char>& vchSig, uint32_t test_case) const
{
	if (!fValid)
	{
		return false;
	}

	vchSig.resize(72);
	size_t nSigLen = 72;
	unsigned char extra_entropy[32] = {0};

	WriteLE32(extra_entropy, test_case);

	secp256k1_ecdsa_signature sig;

	int ret = secp256k1_ecdsa_sign(
		secp256k1_context_sign,
		&sig,
		hash.begin(),
		begin(),
		secp256k1_nonce_function_rfc6979,
		test_case ? extra_entropy : NULL
	);

	assert(ret);

	secp256k1_ecdsa_signature_serialize_der(secp256k1_context_sign, (unsigned char*)&vchSig[0], &nSigLen, &sig);

	vchSig.resize(nSigLen);

	return true;
}

bool CKey::SignCompact(const uint256 &hash, std::vector<unsigned char>& vchSig) const
{
	if (!fValid)
	{
		return false;
	}

	vchSig.resize(65);

	int rec = -1;

	secp256k1_ecdsa_recoverable_signature sig;

	int ret = secp256k1_ecdsa_sign_recoverable(
		secp256k1_context_sign,
		&sig,
		hash.begin(),
		begin(),
		secp256k1_nonce_function_rfc6979,
		NULL
	);

	assert(ret);

	secp256k1_ecdsa_recoverable_signature_serialize_compact(
		secp256k1_context_sign,
		(unsigned char*)&vchSig[1],
		&rec,
		&sig
	);

	assert(ret);
	assert(rec != -1);

	vchSig[0] = 27 + rec + (fCompressed ? 4 : 0);

	return true;
}

bool CKey::Derive(CKey& keyChild, unsigned char ccChild[32], unsigned int nChild, const unsigned char cc[32]) const
{
	assert(IsValid());
	assert(IsCompressed());

	unsigned char out[64];
	LockObject(out);

	if ((nChild >> 31) == 0)
	{
		CPubKey pubkey = GetPubKey();
		
		assert(pubkey.begin() + 33 == pubkey.end());
		
		BIP32Hash(cc, nChild, *pubkey.begin(), pubkey.begin()+1, out);
	}
	else
	{
		assert(begin() + 32 == end());
		
		BIP32Hash(cc, nChild, 0, begin(), out);
	}

	memcpy(ccChild, out+32, 32);
	memcpy((unsigned char*)keyChild.begin(), begin(), 32);

	bool ret = secp256k1_ec_privkey_tweak_add(secp256k1_context_sign, (unsigned char*)keyChild.begin(), out);

	UnlockObject(out);

	keyChild.fCompressed = true;
	keyChild.fValid = ret;

	return ret;
}

bool CKey::VerifyPubKey(const CPubKey& pubkey) const
{
	if (pubkey.IsCompressed() != fCompressed)
	{
		return false;
	}

	unsigned char rnd[8];
	std::string str = "DigitalNote key verification\n";
	GetRandBytes(rnd, sizeof(rnd));
	uint256 hash;
	std::vector<unsigned char> vchSig;

	CHash256().Write((unsigned char*)str.data(), str.size()).Write(rnd, sizeof(rnd)).Finalize(hash.begin());

	Sign(hash, vchSig);

	return pubkey.Verify(hash, vchSig);
}

bool CKey::Load(CPrivKey &privkey, CPubKey &vchPubKey, bool fSkipCheck)
{
	if (!ec_privkey_import_der(secp256k1_context_sign, (unsigned char*)begin(), &privkey[0], privkey.size()))
	{
		return false;
	}

	fCompressed = vchPubKey.IsCompressed();
	fValid = true;

	if (fSkipCheck)
	{
		return true;
	}

	return VerifyPubKey(vchPubKey);
}

/*
	Private
*/
bool CKey::Check(const unsigned char *vch)
{
	return secp256k1_ec_seckey_verify(secp256k1_context_sign, vch);
}

void ECC_Start()
{
	assert(secp256k1_context_sign == NULL);

	secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
	assert(ctx != NULL);

	{
		// Pass in a random blinding seed to the secp256k1 context.
		unsigned char seed[32];
		
		LockObject(seed);
		GetRandBytes(seed, 32);
		
		bool ret = secp256k1_context_randomize(ctx, seed);
		
		assert(ret);
		
		UnlockObject(seed);
	}

	secp256k1_context_sign = ctx;
}

void ECC_Stop()
{
	secp256k1_context *ctx = secp256k1_context_sign;
	secp256k1_context_sign = NULL;

	if (ctx)
	{
		secp256k1_context_destroy(ctx);
	}
}

bool ECC_InitSanityCheck()
{
	CKey key;

	key.MakeNewKey(true);

	CPubKey pubkey = key.GetPubKey();

	return key.VerifyPubKey(pubkey);
}

