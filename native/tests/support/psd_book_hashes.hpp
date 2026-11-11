#pragma once

// The image-way PSD (the default PSD) of each page of the PSD book (native/tests/data/psd_text_book.json) at 150 dpi:
// the sha256 of each file. Pages 2-4 as e146b96's genko wrote them; page 1 since M5①, where the book's
// 「大きな声で約束する」 has its rubies on 「声」 and 「約束」. test_psd_text's imageWayIsAsBefore holds this build to all four.
namespace genko::test {

inline constexpr const char* kPsdBookImageWay[4] = {
    "3bb689e02a4ee5833f89b3eefc0c4894adf821bdc7ff004313438efca8f3c2ba",
    "1f1457dfff7b5c7782f9d8532a37d242d9385a714dd26f97dd0199dd1cc99ca4",
    "f7d33547e0049c31cfd8ec16e5128a01dbca3a7aeecbb42155dfced6fcda7af7",
    "5a8b99f44a399f5542db7ced98d789069da421975dcb7f81097110d5bb3fe665",
};

// (PC5 G-e, the review's missing test (6)) The editable PSD (文字を編集可能なレイヤーとして保持) of each page of the PSD book
// at 150 dpi: the sha256 of each file, recorded by the G-e build — whose lines' text layers write the keys and values
// PSD-01 wrote (the review read them the same as e806f99's) — so that a later change to the text engine's writer is seen.
inline constexpr const char* kPsdBookEditable[4] = {
    "acefa57c335abcef2256b3c75f8c338352aa9f72209ab2389576e99a19c662ed",
    "4bb9cc91441ebf33d72cd2e7a17daacb730401e880a80c4da42ae1c12989599e",
    "0538d1478649cf1f4b510d8360d7f2b9dbccdf4aaebfdd1513d38ba40fc83783",
    "283d5adea87671b7970d4a81fef81139f9a683ece239a95b33848e15eb6e6526",
};

}  // namespace genko::test
