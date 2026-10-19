//------------------------------------------------------------------------------
// Tests for non-RSA (EC) keys in XrdCryptosslRSA, as used by GSI for signing
// DH parameters and random tags, see src/XrdCrypto/XrdCryptosslRSA.cc
//------------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "XrdCrypto/XrdCryptosslAux.hh"
#include "XrdCrypto/XrdCryptosslRSA.hh"
#include "XrdOuc/XrdOucString.hh"
#include "XrdSut/XrdSutBucket.hh"

namespace {

EVP_PKEY *GenKey(int id, int param)
{
   EVP_PKEY *key = 0;
   EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(id, 0);
   if (!ctx || EVP_PKEY_keygen_init(ctx) != 1) return 0;
   if (id == EVP_PKEY_EC)
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, param);
   else
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, param);
   EVP_PKEY_keygen(ctx, &key);
   EVP_PKEY_CTX_free(ctx);
   return key;
}

// Public-only copy, as a peer would get it from the certificate
std::unique_ptr<XrdCryptosslRSA> PublicCopy(XrdCryptoRSA &k)
{
   XrdOucString pub;
   if (k.ExportPublic(pub) < 0) return nullptr;
   return std::make_unique<XrdCryptosslRSA>(pub.c_str(), pub.length());
}

// The bucket based calls are the ones used by GSI
int Sign(XrdCryptoRSA &k, XrdSutBucket &b) { return k.EncryptPrivate(b); }
int Verify(XrdCryptoRSA &k, XrdSutBucket &b) { return k.DecryptPublic(b); }

struct Bucket : public XrdSutBucket {
   Bucket(const void *b, int l) { SetBuf((const char *) b, l); }
};

const char kMsg[] = "server DH public parameters";

} // namespace

class XrdCryptosslECKey : public ::testing::TestWithParam<int> {};

TEST_P(XrdCryptosslECKey, SignVerifyRoundTrip)
{
   XrdCryptosslRSA key(GenKey(EVP_PKEY_EC, GetParam()));
   ASSERT_EQ(key.status, XrdCryptoRSA::kComplete);
   EXPECT_FALSE(XrdCryptosslKeyIsRSA(&key));

   Bucket bck(kMsg, sizeof(kMsg));
   ASSERT_GT(Sign(key, bck), (int) sizeof(kMsg));

   auto pub = PublicCopy(key);
   ASSERT_TRUE(pub && pub->status == XrdCryptoRSA::kPublic);
   ASSERT_EQ(Verify(*pub, bck), (int) sizeof(kMsg));
   EXPECT_EQ(memcmp(bck.buffer, kMsg, sizeof(kMsg)), 0);
}

TEST_P(XrdCryptosslECKey, CopyKeepsPrivateKey)
{
   // The GSI handshake signs with copies of the credential key
   XrdCryptosslRSA key(GenKey(EVP_PKEY_EC, GetParam()));
   ASSERT_EQ(key.status, XrdCryptoRSA::kComplete);
   XrdCryptosslRSA cpy(key);
   EXPECT_EQ(cpy.status, XrdCryptoRSA::kComplete);

   Bucket bck(kMsg, sizeof(kMsg));
   EXPECT_GT(Sign(cpy, bck), 0);

   // and verifies with copies of the peer public key
   auto pub = PublicCopy(key);
   ASSERT_TRUE(pub);
   XrdCryptosslRSA pubcpy(*pub);
   EXPECT_EQ(pubcpy.status, XrdCryptoRSA::kPublic);
   EXPECT_EQ(Verify(pubcpy, bck), (int) sizeof(kMsg));
}

TEST_P(XrdCryptosslECKey, TamperedSignatureIsRejected)
{
   XrdCryptosslRSA key(GenKey(EVP_PKEY_EC, GetParam()));
   auto pub = PublicCopy(key);
   ASSERT_TRUE(pub);

   Bucket sig(kMsg, sizeof(kMsg));
   int lsig = Sign(key, sig);
   ASSERT_GT(lsig, 0);
   std::string good(sig.buffer, sig.size);

   // Altered message
   std::string bad = good;
   bad[4] ^= 0x01;
   Bucket b1(bad.data(), bad.size());
   EXPECT_EQ(Verify(*pub, b1), -1);

   // Message length pointing past the end, or leaving no room for a signature
   for (unsigned char l : {0x00, 0xff}) {
      bad = good;
      bad[0] = bad[1] = bad[2] = bad[3] = (char) l;
      Bucket b2(bad.data(), bad.size());
      EXPECT_EQ(Verify(*pub, b2), -1);
   }

   // Truncated
   Bucket b3(good.data(), 3);
   EXPECT_EQ(Verify(*pub, b3), -1);

   // Signed by another key
   XrdCryptosslRSA other(GenKey(EVP_PKEY_EC, GetParam()));
   Bucket b4(kMsg, sizeof(kMsg));
   ASSERT_GT(Sign(other, b4), 0);
   EXPECT_EQ(Verify(*pub, b4), -1);
}

INSTANTIATE_TEST_SUITE_P(Curves, XrdCryptosslECKey,
   ::testing::Values(NID_X9_62_prime256v1, NID_secp384r1, NID_secp521r1));

TEST(XrdCryptosslRSAKey, SignVerifyRoundTripUnchanged)
{
   XrdCryptosslRSA key(GenKey(EVP_PKEY_RSA, 2048));
   ASSERT_EQ(key.status, XrdCryptoRSA::kComplete);
   EXPECT_TRUE(XrdCryptosslKeyIsRSA(&key));

   // RSA keys keep signing with message recovery, as expected by older peers
   Bucket bck(kMsg, sizeof(kMsg));
   ASSERT_EQ(Sign(key, bck), 256);
   auto pub = PublicCopy(key);
   ASSERT_TRUE(pub);
   ASSERT_EQ(Verify(*pub, bck), (int) sizeof(kMsg));
   EXPECT_EQ(memcmp(bck.buffer, kMsg, sizeof(kMsg)), 0);
}
