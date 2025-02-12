#ifndef STEALTH_H
#define STEALTH_H

#include <cstdint>
#include <string>
#include <vector>

#include "types/ec_point.h"

const uint32_t MAX_STEALTH_NARRATION_SIZE = 48;
const size_t ec_secret_size = 32;
const size_t ec_compressed_size = 33;
const size_t ec_uncompressed_size = 65;
const uint8_t stealth_version_byte = 0x28;

typedef struct ec_secret
{
	uint8_t e[ec_secret_size];
} ec_secret;

typedef uint32_t stealth_bitfield;

struct stealth_prefix
{
    uint8_t number_bits;
    stealth_bitfield bitfield;
};

template <typename T, typename Iterator>
T from_big_endian(Iterator in);

template <typename T, typename Iterator>
T from_little_endian(Iterator in);

void AppendChecksum(data_chunk& data);
bool VerifyChecksum(const data_chunk& data);
int GenerateRandomSecret(ec_secret& out);
int SecretToPublicKey(const ec_secret& secret, ec_point& out);
int StealthSecret(ec_secret& secret, ec_point& pubkey, const ec_point& pkSpend, ec_secret& sharedSOut, ec_point& pkOut);
int StealthSecretSpend(ec_secret& scanSecret, ec_point& ephemPubkey, ec_secret& spendSecret, ec_secret& secretOut);
int StealthSharedToSecretSpend(ec_secret& sharedS, ec_secret& spendSecret, ec_secret& secretOut);
bool IsStealthAddress(const std::string& encodedAddress);

#endif  // STEALTH_H

