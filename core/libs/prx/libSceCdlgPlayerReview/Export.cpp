#include "prx/libc/include/General.hpp"
#include <cstdint>
#include <exception>

// FAIL-CLOSED DIAGNOSTIC ONLY. These shapes come from the observed caller,
// not an SDK contract. No dialog, service, result, error or lifecycle is emulated.
extern "C" {
std::int32_t APS5_VABI scePlayerReviewDialogInitialize() {
    NotImplemented_nid_no_patch("DIAGNOSTIC ONLY: scePlayerReviewDialogInitialize [UtXl-tmi7iw]; SDK contract unavailable");
    std::terminate();
}
std::int32_t APS5_VABI scePlayerReviewDialogOpen(const void*) {
    NotImplemented_nid_no_patch("DIAGNOSTIC ONLY: scePlayerReviewDialogOpen [CtygVRCL+bA]; SDK contract unavailable");
    std::terminate();
}
std::int32_t APS5_VABI scePlayerReviewDialogUpdateStatus() {
    NotImplemented_nid_no_patch("DIAGNOSTIC ONLY: scePlayerReviewDialogUpdateStatus [4J5F23VgTjY]; SDK contract unavailable");
    std::terminate();
}
std::int32_t APS5_VABI scePlayerReviewDialogGetResult(void*) {
    NotImplemented_nid_no_patch("DIAGNOSTIC ONLY: scePlayerReviewDialogGetResult [VuKbx6zlEG4]; SDK contract unavailable");
    std::terminate();
}
std::int32_t APS5_VABI scePlayerReviewDialogTerminate() {
    NotImplemented_nid_no_patch("DIAGNOSTIC ONLY: scePlayerReviewDialogTerminate [RmJKkzZFNFA]; SDK contract unavailable");
    std::terminate();
}
}
