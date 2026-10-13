#pragma once
// Pinned embedding artifacts. CMake reads these string literals; keep each
// value on one line. Configure-time fetch and the first-run fetch share
// this file so the checksums cannot drift.
//
// Model: Xenova/bge-small-en-v1.5 @ ea104dacec62c0de699686887e3f920caeb4f3e3
// onnx/model_quantized.onnx — int8 BERT graph, 34,014,426 bytes.
// vocab.txt — BERT WordPiece, 30,522 tokens, same revision.

namespace arbiter {

inline constexpr const char* kEmbedModelUrl = "https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/ea104dacec62c0de699686887e3f920caeb4f3e3/onnx/model_quantized.onnx";
inline constexpr const char* kEmbedModelSha256 = "6c9c6101a956d62dfb5e7190c538226c0c5bb9cb27b651234b6df063ee7dbfe4";
inline constexpr const char* kEmbedVocabUrl = "https://huggingface.co/Xenova/bge-small-en-v1.5/resolve/ea104dacec62c0de699686887e3f920caeb4f3e3/vocab.txt";
inline constexpr const char* kEmbedVocabSha256 = "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3";

inline constexpr const char* kOnnxRuntimeVersion = "1.31.0";
inline constexpr const char* kOnnxRuntimeLinuxX64Url = "https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-linux-x64-1.31.0.tgz";
inline constexpr const char* kOnnxRuntimeLinuxX64Sha256 = "cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57";
inline constexpr const char* kOnnxRuntimeOsxArm64Url = "https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/onnxruntime-osx-arm64-1.31.0.tgz";
inline constexpr const char* kOnnxRuntimeOsxArm64Sha256 = "1031b36dd3bdaa8d976a6130d9cf2a05e9be320b93ae804f8427b91dc3432afc";

} // namespace arbiter
