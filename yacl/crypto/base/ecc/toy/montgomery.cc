// Copyright 2023 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yacl/crypto/base/ecc/toy/montgomery.h"

#include "absl/strings/escaping.h"

#include "yacl/crypto/base/hash/ssl_hash.h"

namespace yacl::crypto::toy {

// Scalars are assumed to be randomly generated bytes.  For X25519, in
// order to decode 32 random bytes as an integer scalar, set the three
// least significant bits of the first byte and the most significant bit
// of the last to zero, set the second most significant bit of the last
// byte to 1 and, finally, decode as little-endian.  This means that the
// resulting integer is of the form 2^254 plus eight times a value
// between 0 and 2^251 - 1 (inclusive).
void MaskScalar255(MPInt *scalar) {
  scalar->SetBit(0, 0);
  scalar->SetBit(1, 0);
  scalar->SetBit(2, 0);
  scalar->SetBit(255, 0);
  scalar->SetBit(254, 1);
}

void MaskPoint255(MPInt *p) { p->SetBit(255, 0); }

ToyXGroup::ToyXGroup(const CurveMeta &curve_meta, const CurveParam &param)
    : ToyEcGroup(curve_meta, param) {
  a24_ = (params_.A - 2_mp) / 4_mp;
}

std::string ToyXGroup::ToString() {
  return fmt::format("{} ==> y^2 = x^3 + {}x^2 + x (mod {})", GetCurveName(),
                     params_.A, params_.p);
}

EcPoint ToyXGroup::Add(const EcPoint &p1, const EcPoint &p2) const {
  YACL_THROW(
      "{} from {} do not support Add, because p1, p2 only has X-coordinate",
      GetCurveName(), GetLibraryName());
}

// Conditional swap in constant time.
void cswap(int swap, MPInt *x_2, MPInt *x_3) {
  auto dummy = MPInt(swap) * (*x_2 - *x_3);
  *x_2 -= dummy;
  *x_3 += dummy;
}

EcPoint ToyXGroup::Mul(const EcPoint &point, const MPInt &k) const {
  auto scalar = k;
  MaskScalar255(&scalar);

  auto x_1 = std::get<AffinePoint>(point).x;
  MaskPoint255(&x_1);

  auto x_2 = MPInt(1);
  auto z_2 = MPInt(0);
  auto x_3 = x_1;
  auto z_3 = x_2;
  int8_t swap = 0;

  for (int t = params_.p.BitCount() - 1; t >= 0; --t) {
    int8_t k_t = scalar[t];
    swap ^= k_t;
    cswap(swap, &x_2, &x_3);
    cswap(swap, &z_2, &z_3);
    swap = k_t;

    auto A = x_2 + z_2;
    auto AA = A.MulMod(A, params_.p);
    auto B = x_2 - z_2;
    auto BB = B.MulMod(B, params_.p);
    auto E = AA - BB;
    auto C = x_3 + z_3;
    auto D = x_3 - z_3;
    auto DA = D.MulMod(A, params_.p);
    auto CB = C.MulMod(B, params_.p);
    x_3 = (DA + CB).PowMod(2_mp, params_.p);
    z_3 = x_1.MulMod((DA - CB).Pow(2), params_.p);
    x_2 = AA.MulMod(BB, params_.p);
    z_2 = E.MulMod(AA + a24_ * E, params_.p);
  }

  cswap(swap, &x_2, &x_3);
  cswap(swap, &z_2, &z_3);
  auto res = x_2.MulMod(z_2.PowMod(params_.p - 2_mp, params_.p), params_.p);
  return AffinePoint(res, {});
}

EcPoint ToyXGroup::Negate(const EcPoint &point) const {
  return Mul(point, params_.n - 1_mp);
}

Buffer ToyXGroup::SerializePoint(const EcPoint &point,
                                 PointOctetFormat format) const {
  YACL_ENFORCE(format == PointOctetFormat::Autonomous,
               "Toy lib do not support {} format", (int)format);
  const auto &op = std::get<AffinePoint>(point);
  return op.x.Serialize();
}

void ToyXGroup::SerializePoint(const EcPoint &point, PointOctetFormat format,
                               Buffer *buf) const {
  *buf = SerializePoint(point, format);
}

EcPoint ToyXGroup::DeserializePoint(ByteContainerView buf,
                                    PointOctetFormat format) const {
  YACL_ENFORCE(format == PointOctetFormat::Autonomous,
               "Toy lib do not support {} format", (int)format);
  AffinePoint op;
  op.x.Deserialize(buf);
  return op;
}

EcPoint ToyXGroup::HashToCurve(HashToCurveStrategy strategy,
                               std::string_view str) const {
  auto bits = params_.p.BitCount();
  HashAlgorithm hash_algorithm;
  switch (strategy) {
    case HashToCurveStrategy::HashAsPointX_SHA2:
      if (bits <= 224) {
        hash_algorithm = HashAlgorithm::SHA224;
      } else if (bits <= 256) {
        hash_algorithm = HashAlgorithm::SHA256;
      } else if (bits <= 384) {
        hash_algorithm = HashAlgorithm::SHA384;
      } else {
        hash_algorithm = HashAlgorithm::SHA512;
      }
      break;
    case HashToCurveStrategy::HashAsPointX_SHA3:
      YACL_THROW("Openssl lib do not support HashAsPointX_SHA3 strategy now");
      break;
    case HashToCurveStrategy::HashAsPointX_SM:
      hash_algorithm = HashAlgorithm::SM3;
      break;
    default:
      YACL_THROW(
          "Openssl lib only support HashAsPointX strategy now. select={}",
          (int)strategy);
  }

  auto buf = SslHash(hash_algorithm).Update(str).CumulativeHash();

  AffinePoint op;
  op.x.Set(
      absl::BytesToHexString(absl::string_view((char *)buf.data(), buf.size())),
      16);
  return op;
}

bool ToyXGroup::PointEqual(const EcPoint &p1, const EcPoint &p2) const {
  return std::get<AffinePoint>(p1).x == std::get<AffinePoint>(p2).x;
}

// Check point is on Curve25519 or on any twist of Curve25519
// C25519: y^2 = x^3 + 486662x^2 + x; order = 8q
// Twist of C25519: y^2 / t = x^3 + 486662x^2 + x; order = 4r
// In the ECDH scenario we do not distinguish between points on C25519 or on a
// twist curve
bool ToyXGroup::IsInCurveGroup(const EcPoint &point) const {
  auto &x = std::get<AffinePoint>(point).x;
  return !x.IsNegative() && x < params_.p;
}

bool ToyXGroup::IsInfinity(const EcPoint &point) const {
  return std::get<AffinePoint>(point).x.IsZero();
}

}  // namespace yacl::crypto::toy
