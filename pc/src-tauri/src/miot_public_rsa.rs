//! MIoCo's fixed public-key envelope, RFC 8017 section 7.2.1.
//! Only public RSA encryption is implemented: no private key, decrypt, sign,
//! key generation, or caller-selectable private exponent. Keep this independent
//! of the `rsa` crate (CVE-2023-49092 has no patched release).
use base64::{engine::general_purpose::STANDARD, Engine};
use num_bigint_dig::BigUint;
use pkcs1::der::Decode;
use rand::{CryptoRng, RngCore};
use zeroize::Zeroizing;

const ERROR: &str = "家居协议初始化失败";

pub(super) fn parse_public_key(pem: &str) -> Result<(BigUint, BigUint), String> {
    if pem.len() > 4096 { return Err(ERROR.into()); }
    let lines: Vec<_> = pem.trim().lines().map(str::trim).collect();
    if lines.len() < 3 || lines[0] != "-----BEGIN PUBLIC KEY-----"
        || lines[lines.len() - 1] != "-----END PUBLIC KEY-----"
    { return Err(ERROR.into()); }
    let der = STANDARD.decode(lines[1..lines.len()-1].concat()).map_err(|_| ERROR)?;
    let info = spki::SubjectPublicKeyInfoRef::from_der(&der).map_err(|_| ERROR)?;
    if info.algorithm.oid != spki::ObjectIdentifier::new_unwrap("1.2.840.113549.1.1.1") {
        return Err(ERROR.into());
    }
    let bytes = info.subject_public_key.as_bytes().ok_or(ERROR)?;
    let public = pkcs1::RsaPublicKey::from_der(bytes).map_err(|_| ERROR)?;
    let n = BigUint::from_bytes_be(public.modulus.as_bytes());
    let e = BigUint::from_bytes_be(public.public_exponent.as_bytes());
    if !(1024..=4096).contains(&n.bits()) || public.modulus.as_bytes().last().map(|b| b & 1) != Some(1)
        || public.public_exponent.as_bytes().len() > 4 || e < BigUint::from(3u8)
        || public.public_exponent.as_bytes().last().map(|b| b & 1) != Some(1)
    { return Err(ERROR.into()); }
    Ok((n, e))
}

pub(super) fn wrap_session_key<R: RngCore + CryptoRng>(
    public_pem: &str, key: &[u8; 16], rng: &mut R,
) -> Result<Vec<u8>, String> {
    let (n, e) = parse_public_key(public_pem)?;
    let size = (n.bits() + 7) / 8;
    let delimiter = size.checked_sub(key.len() + 1).ok_or(ERROR)?;
    if delimiter < 10 { return Err(ERROR.into()); } // PS must contain at least 8 bytes.
    let mut encoded = Zeroizing::new(vec![0u8; size]);
    encoded[1] = 2;
    let padding = &mut encoded[2..delimiter];
    rng.try_fill_bytes(padding).map_err(|_| ERROR)?;
    for byte in padding {
        // Rejection sampling, never remap zero to a biased nonzero value.
        // A broken RNG fails closed instead of looping indefinitely.
        let mut retries = 0;
        while *byte == 0 {
            if retries == 128 { return Err(ERROR.into()); }
            rng.try_fill_bytes(std::slice::from_mut(byte)).map_err(|_| ERROR)?;
            retries += 1;
        }
    }
    encoded[delimiter] = 0;
    encoded[delimiter + 1..].copy_from_slice(key);
    let message = Zeroizing::new(BigUint::from_bytes_be(&encoded));
    if *message >= n { return Err(ERROR.into()); }
    let encrypted = message.modpow(&e, &n).to_bytes_be();
    if encrypted.len() > size { return Err(ERROR.into()); }
    // I2OSP is fixed-width, including leading-zero ciphertext bytes.
    let mut ciphertext = vec![0u8; size];
    ciphertext[size - encrypted.len()..].copy_from_slice(&encrypted);
    Ok(ciphertext)
}
