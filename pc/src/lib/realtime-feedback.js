// An abandoned ASR turn is not a failed conversation. Do not hide other errors.
export function isRecoveringAsr(status) {
  return status?.active === true && status?.state === "listening"
    && status?.reason === "asr_timeout";
}
