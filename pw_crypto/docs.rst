.. _module-pw_crypto:

=========
pw_crypto
=========
.. pigweed-module::
   :name: pw_crypto

A set of safe (read: easy to use, hard to misuse) crypto APIs.

The following crypto services are provided by this module.

1. Hashing a message with `SHA256`_.
2. Verifying a digital signature signed with `ECDSA`_ over the NIST P256 curve.
3. Symmetric encryption and decryption with `AES`_.
4. Key agreement using `ECDH`_.
5. Stream encryption/decryption with `ChaCha20`_.
6. Many more to come ...

------
SHA256
------
1. Obtaining a oneshot digest.

.. code-block:: cpp

   #include "pw_crypto/sha256.h"

   std::byte digest[32];
   if (!pw::crypto::sha256::Hash(message, digest).ok()) {
     // Handle errors.
   }

   // The content can also come from a pw::stream::Reader.
   if (!pw::crypto::sha256::Hash(reader, digest).ok()) {
     // Handle errors.
   }

2. Hashing a long, potentially non-contiguous message.

.. code-block:: cpp

   #include "pw_crypto/sha256.h"

   std::byte digest[32];

   if (!pw::crypto::sha256::Sha256()
            .Update(chunk1)
            .Update(chunk2)
            .Update(chunk...)
            .Final()
            .ok()) {
     // Handle errors.
   }

-----
ECDSA
-----
1. Verifying a digital signature signed with ECDSA over the NIST P256 curve.

.. code-block:: cpp

   #include "pw_crypto/ecdsa.h"
   #include "pw_crypto/sha256.h"

   std::byte digest[32];
   if (!pw::crypto::sha256::Hash(message, digest).ok()) {
     // handle errors.
   }

   if (!pw::crypto::ecdsa::VerifyP256Signature(public_key, digest, signature)
            .ok()) {
     // handle errors.
   }

2. Verifying a digital signature signed with ECDSA over the NIST P256 curve,
   with a long and/or non-contiguous message.

.. code-block:: cpp

   #include "pw_crypto/ecdsa.h"
   #include "pw_crypto/sha256.h"

   std::byte digest[32];

   if (!pw::crypto::sha256::Sha256()
            .Update(chunk1)
            .Update(chunk2)
            .Update(chunkN)
            .Final(digest)
            .ok()) {
     // Handle errors.
   }

   if (!pw::crypto::ecdsa::VerifyP256Signature(public_key, digest, signature)
            .ok()) {
     // Handle errors.
   }

---
AES
---

1. Computing the AES-CMAC of a potentially long and/or non-contiguous message.
   This is similar to a hash or digest except that the operation takes a secret
   key as an input, so the MAC can be used to verify integrity and
   authentication.

.. code-block:: cpp

   #include "pw_crypto/aes.h"

   std::byte mac[16];

   if (!pw::crypto::aes_cmac::Cmac(key)
            .Update(chunk1)
            .Update(chunk2)
            .Update(chunk...)
            .Final()
            .ok()) {
     // Handle errors.
   }

2. Encrypting a single AES 128-bit block.

.. warning::
  This is a low-level operation. Users should know exactly what they are doing
  and must ensure that this operation does not violate any safety bounds that
  more refined operations usually ensure.

.. code-block:: cpp

   #include "pw_crypto/aes.h"

   std::byte encrypted[16];

   if (!pw::crypto::unsafe::aes::EncryptBlock(key, message, encrypted).ok()) {
     // Handle errors.
   }

----
ECDH
----
1. Generating a keypair and computing a shared symmetric key.

.. warning::
   Ensure that the backend is initialized and configured correctly with a
   cryptographically secure pseudo-random number generator (CSPRNG). The details
   for doing this are specific to each backend.

.. code-block:: cpp

   #include "pw_crypto/ecdh.h"

   // Import the public key from the other party.
   PW_TRY_ASSIGN(auto public_key,
                 pw::crypto::ecdh::P256PublicKey::Import(other_x,
                                                         other_y,
                                                         endian));
   PW_TRY_ASSIGN(auto keypair, pw::crypto::ecdh::P256Keypair::Generate());

   std::byte shared_key[32];
   if (!keypair.ComputeDiffieHellman(public_key, shared_key)) {
     // handle errors.
   }

2. Import a pre-existing keypair (for testing purposes) and computing a
   shared symmetric key.

.. code-block:: cpp

   #include "pw_crypto/ecdh.h"

   // Import the public key from the other party.
   PW_TRY_ASSIGN(auto public_key,
                 pw::crypto::ecdh::P256PublicKey::Import(other_x,
                                                         other_y,
                                                         endian));
   PW_TRY_ASSIGN(
       auto keypair,
       pw::crypto::ecdh::P256Keypair::ImportForTesting(private_key, x, y, endian));

   std::byte shared_key[32];
   if (!keypair.ComputeDiffieHellman(public_key, shared_key)) {
     // handle errors.
   }

--------
ChaCha20
--------
1. Encrypting or decrypting a message in a oneshot manner. ChaCha20 is a stream
   cipher, so the same operation is used for both encryption and decryption.
   This API is intended for single invocations where a unique (key, nonce) pair
   is used per message, typically starting with an initial counter of 1.

.. warning::
   It is CRITICAL to never reuse the same (key, nonce) pair for different
   messages. Doing so will compromise the confidentiality of all messages
   encrypted with that pair.

.. code-block:: cpp

   #include <vector>

   #include "pw_bytes/span.h"
   #include "pw_crypto/chacha20.h"

   constexpr std::byte key[32] = ...;    // Your 256-bit secret key
   constexpr std::byte nonce[12] = ...;  // Your 96-bit unique nonce
   constexpr uint32_t counter = 1;       // Initial counter value

   std::vector<std::byte> plaintext = ...;
   std::vector<std::byte> ciphertext(plaintext.size());

   // Encrypt
   pw::Status status = pw::crypto::chacha20::Crypt(
       pw::span(key), pw::span(nonce), counter, plaintext, pw::span(ciphertext));

   if (!status.ok()) {
     // Handle encryption error
   }

   // Decrypt (same function)
   std::vector<std::byte> decrypted(ciphertext.size());
   status = pw::crypto::chacha20::Crypt(
       pw::span(key), pw::span(nonce), counter, ciphertext, pw::span(decrypted));

   if (!status.ok()) {
     // Handle decryption error
   }
   // decrypted should now equal plaintext

-------------
Configuration
-------------
The crypto services offered by pw_crypto can be backed by different backend
crypto libraries.

MbedTLS
=======
The `MbedTLS project <https://www.trustedfirmware.org/projects/mbed-tls/>`_
is a mature and full-featured crypto library that implements cryptographic
primitives, X.509 certificate manipulation and the SSL/TLS and DTLS protocols.

The project also has good support for interfacing to cryptographic accelerators.

The small code footprint makes the project suitable and popular for embedded
systems.

Enabling the MbedTLS Backend
----------------------------

To select the MbedTLS backend, the MbedTLS library needs to be installed and
configured.

GN
^^

.. code-block:: sh

   # Install and configure MbedTLS
   pw package install mbedtls
   gn gen out --args='
       dir_pw_third_party_mbedtls=getenv("PW_PACKAGE_ROOT")+"/mbedtls"
       pw_external_mbedtls=getenv("PW_PROJECT_ROOT")+"/third_party/mbedtls"
       pw_crypto_SHA256_BACKEND="//pw_crypto:sha256_mbedtls_v3"
       pw_crypto_ECDSA_BACKEND="//pw_crypto:ecdsa_mbedtls_v3"
       pw_crypto_AES_BACKEND="//pw_crypto:aes_mbedtls_v3"
       pw_crypto_ECDH_BACKEND="//pw_crypto:ecdh_mbedtls_v3"
       pw_crypto_CHACHA20_BACKEND="//pw_crypto:chacha20_mbedtls_v3"
   '

   ninja -C out

Bazel
^^^^^

If using Bazel, add a ``bazel_dep`` on MbedTLS to your ``MODULE.bazel`` file
and select appropriate backends by adding them to your project's `platform
<https://bazel.build/extending/platforms>`_:

.. code-block:: python

   platform(
     name = "my_platform",
     flags = [
        "@pigweed//pw_crypto:sha256_backend=@pigweed//pw_crypto:sha256_mbedtls",
        "@pigweed//pw_crypto:ecdsa_backend=@pigweed//pw_crypto:ecdsa_mbedtls",
        "@pigweed//pw_crypto:aes_backend=@pigweed//pw_crypto:aes_mbedtls",
        "@pigweed//pw_crypto:ecdh_backend=@pigweed//pw_crypto:ecdh_mbedtls",
        "@pigweed//pw_crypto:chacha20_backend=@pigweed//pw_crypto:chacha20_mbedtls",
        # ... other flags
      ],
   )

Configuring MbedTLS
-------------------

MbedTLS is configured via setting compile-time flags in a `config.h` header.
These flags are enumerated in the `MbedTLS docs <https://mbed-tls.readthedocs.io/projects/api/en/v3.6.0/api/file/mbedtls__config_8h/>`_.

By default, Pigweed provides a default MbedTLS configuration for both MCU and
host targets. However, projects making use of MbedTLS should strongly consider
providing their own config.

Bazel
^^^^^

The config file is set via the ``mbedtls//:mbedtls_config`` label flag.

1. Create your ``config.h`` based on Pigweed's default config in ``third_party/mbedtls/config_pigweed.h``, or MbedTLS's `example configs <https://github.com/Mbed-TLS/mbedtls/tree/development/configs>`_.

2. In your BUILD.bazel, define a library for your header:

.. code-block:: python

   cc_library(
       name = "mbedtls_config",
       hdrs = [
           "my_config/my_mbedtls_config.h",
       ],
       strip_include_prefix = "my_config",
       tags = ["noclangtidy"],
       defines = [
           "MBEDTLS_CONFIG_FILE='\"my_mbedtls_config.h\"'",
       ]
   )

3. Set the label flag in your project's platform or `.bazelrc`:

.. code-block:: python

   ...
   "@@mbedtls+//:mbedtls_config": "@mymodule//crypto:mbedtls_config",
   ...

Hardware Acceleration
---------------------

MbedTLS supports linking against `alternative implementations
<https://mbed-tls.readthedocs.io/en/latest/kb/development/hw_acc_guidelines/>`_
for many modules, such as entropy, AES, and SHA-2. These ``ALT`` functions are
typically used to provide hardware-accelerated cryptography engines.

Bazel
^^^^^

1.  Build your implementation against MbedTLS headers:

.. code-block:: python

   cc_library(
       name = "sha256_alt",
       hdrs = [ "sha256_alt.h" ],
       deps = [
           "@pigweed//targets:mcuxpresso_sdk",
       ]
   )

   cc_library(
       name = "mbedtls_sha256_hashcrypt",
       srcs = [
           "mbedtls_sha256_hashcrypt.cc",
       ],
       deps = [
           ":sha256_alt",
           "@mbedtls",
           "@pigweed//targets:mcuxpresso_sdk",
       ]
   )

2. Modify your MbedTLS config.h:

.. code-block:: cpp

   ...
   #define MBEDTLS_SHA256_C
   #define MBEDTLS_SHA256_ALT
       ...

3. Set platform label flags:

.. code-block:: python

   ...
   "@@mbedtls+//:mbedtls_config": "@mymodule//crypto:mbedtls_config",
   "@pigweed//pw_crypto:sha256_backend": "@pigweed//pw_crypto:sha256_mbedtls",
   "@pigweed//pw_crypto:mbedtls_sha256_engine": "@mymodule//crypto:mbedtls_sha256_hashcrypt",
   ...

Configuration Tips
------------------

For optimal code size and/or performance, the MbedTLS library can be configured
per product. MbedTLS configuration is achieved by turning on and off MBEDTLS_*
options in a config.h file. See //third_party/mbedtls for how this is done.

``pw::crypto::sha256`` does not need any special configuration as it uses the
mbedtls_sha256_* APIs directly. However you can optionally turn on
``MBEDTLS_SHA256_SMALLER`` to further reduce the code size to from 3KiB to
~1.8KiB at a ~30% slowdown cost (Cortex-M4).

.. code-block:: c

   #define MBEDTLS_SHA256_SMALLER

``pw::crypto::ecdsa`` requires the following minimum configurations which yields
a code size of ~12KiB.

.. code-block:: c

   #define MBEDTLS_BIGNUM_C
   #define MBEDTLS_ECP_C
   #define MBEDTLS_ECDSA_C
   // The ASN1 options are needed only because mbedtls considers and verifies
   // them (in check_config.h) as dependencies of MBEDTLS_ECDSA_C.
   #define MBEDTLS_ASN1_WRITE_C
   #define MBEDTLS_ASN1_PARSE_C
   #define MBEDTLS_ECP_NO_INTERNAL_RNG
   #define MBEDTLS_ECP_DP_SECP256R1_ENABLED

If using ``pw::crypto::ecdh``, a CSPRNG must be set to provide
cryptographically-secure randomness when generating keypairs. To do this,
provide an instance of ``pw::crypto::ecdh::backend::Csprng`` to
``pw::crypto::ecdh::backend::SetCsprng()``. MbedTLS MUST have been configured
with an entropy pool that has collected sufficient (>128 bits estimated) entropy
with one or more calls to

.. code-block:: cpp

   mbed_entropy_add_source(&entropy, ...)

Then the following implementation can be used to provide a CTR DRBG as the
CSPRNG for ECDH:

.. code-block:: cpp

   using MbedtlsCtrDrbg =
       ::pw::crypto::ecdh::backend::Wrapper<mbedtls_ctr_drbg_context,
                                            mbedtls_ctr_drbg_init,
                                            mbedtls_ctr_drbg_free>;
   class MbedtlsCsprng final : public ::pw::crypto::ecdh::backend::Csprng {
    public:
     MbedtlsCsprng(mbedtls_entropy_context* entropy,
                   std::string_view personalization_string) {
       PW_CHECK_INT_EQ(0,
                       mbedtls_ctr_drbg_seed(ctr_drbg_.Get(),
                                             mbedtls_entropy_func,
                                             &entropy,
                                             personalization_string.data(),
                                             personalization_string.size()));
     }

     GenerateResult Generate(ByteSpan out) override {
       if (mbedtls_ctr_drbg_random(ctr_drbg_.Get(),
                                   reinterpret_cast<unsigned char*>(out.data()),
                                   out.size()) != 0) {
         return GenerateResult::kFailure;
       }
       return GenerateResult::kSuccess;
     }

    private:
     MbedtlsCtrDrbg ctr_drbg_;
   };

``pw::crypto::chacha20`` requires ``MBEDTLS_CHACHA20_C`` to be defined in the
MbedTLS config.

.. code-block:: c

   #define MBEDTLS_CHACHA20_C

.. _module-pw_crypto-boringssl:

BoringSSL
=========
The BoringSSL project (`source
<https://cs.opensource.google/boringssl/boringssl>`_, `GitHub mirror
<https://github.com/google/boringssl>`_) is a fork of OpenSSL maintained by
Google. It is not especially designed to be embedded-friendly, but it is used as
the SSL library in Chrome, Android, and other apps. It is likely better to use
another backend such as Mbed-TLS for embedded targets unless your project needs
BoringSSL specifically.

To use the BoringSSL backend with a GN project, it needs to be installed and
configured. To do that:

.. code-block:: sh

   # Install and configure BoringSSL
   pw package install boringssl
   gn gen out --args='
       dir_pw_third_party_boringssl=getenv("PW_PACKAGE_ROOT")+"/boringssl"
       pw_crypto_AES_BACKEND="//pw_crypto:aes_boringssl"
       pw_crypto_ECDH_BACKEND="//pw_crypto:ecdh_boringssl"
       pw_crypto_CHACHA20_BACKEND="//pw_crypto:chacha20_boringssl"
   '

   ninja -C out

If using Bazel, add the BoringSSL repository to your ``MODULE.bazel``
and select appropriate backends by adding them to your project's `platform
<https://bazel.build/extending/platforms>`_:

.. code-block:: python

   platform(
     name = "my_platform",
     constraint_values = [
       "@pigweed//pw_aes:aes_boringssl_backend",
       # ... other constraint_values
     ],
   )

------------
Size Reports
------------
Below are size reports for each crypto service. These vary across
configurations.

.. include:: pw_crypto_size_report

-------------
API reference
-------------
Moved: :cc:`pw_crypto`
