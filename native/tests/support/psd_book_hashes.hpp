#pragma once

// The image-way PSD (the default PSD) of each page of the PSD book (native/tests/data/psd_text_book.json) at 150 dpi:
// the sha256 of each file. Pages 2-4 as e146b96's genko wrote them; page 1 since M5①, where the book's
// 「大きな声で約束する」 has its rubies on 「声」 and 「約束」 — the same bytes as the reference Python's PSD of that page
// (test_contract_psd_text's imageWayIsPythons holds them so). test_psd_text's imageWayIsAsBefore holds this build to
// all four.
namespace genko::test {

inline constexpr const char* kPsdBookImageWay[4] = {
    "3bb689e02a4ee5833f89b3eefc0c4894adf821bdc7ff004313438efca8f3c2ba",
    "1f1457dfff7b5c7782f9d8532a37d242d9385a714dd26f97dd0199dd1cc99ca4",
    "f7d33547e0049c31cfd8ec16e5128a01dbca3a7aeecbb42155dfced6fcda7af7",
    "5a8b99f44a399f5542db7ced98d789069da421975dcb7f81097110d5bb3fe665",
};

}  // namespace genko::test
