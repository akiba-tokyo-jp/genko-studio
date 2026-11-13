#pragma once

// The image-way PSD (the default PSD) of each page of the PSD book (native/tests/data/psd_text_book.json) at 150 dpi:
// the sha256 of each file. Recorded by the fukidashi@1 build (SPEC BALLOON-01, the user's decision of 2026-11-13: every
// balloon is drawn clean, so the balloons' pixels — and with them these files — are not those of before; the values
// of before were e146b96's, page 1's since M5①). What is not a balloon is held by the other tests of test_psd_text:
// the layers make the line, the editable PSD's merged picture is the image way's. test_psd_text's imageWayIsAsBefore
// holds this build to all four.
namespace genko::test {

inline constexpr const char* kPsdBookImageWay[4] = {
    "86d7ce94dd1b4b62fa92a3bf566dec261fac5b08c6ede14b8ee712c6ce3a5953",
    "47d2512fe4cacb2d98e4cc80c0d20801a6551b60e8f0decef45af849eff833c9",
    "723bb33bfdc26c9125aa9067b09f8013e103bf478219225d0178df628943e6f3",
    "f99aa93eb1679cdfdf18090e0e929a5a4501bf9c5ba35e38f7908dd32f6d65ee",
};

// (PC5 G-e, the review's missing test (6)) The editable PSD (文字を編集可能なレイヤーとして保持) of each page of the PSD book
// at 150 dpi: the sha256 of each file, so that a later change to the text engine's writer is seen. Recorded by the
// fukidashi@1 build too (the balloon layers' pixels are the clean balloons'; an oval's letters are fitted in the oval
// its true ellipse holds — the book's ops give each box as it was, so that oval is a little smaller than the
// hand-drawn one was; the text layers' keys are written as the G-e build wrote them).
inline constexpr const char* kPsdBookEditable[4] = {
    "0fc13dd7527d199f992e34a31d8360f62151da1124224faefeec74267e674e28",
    "9a9d2b01e8ffec82e74af7a5022ba8d2e84dd0daadc351154ffb44392f4df1af",
    "64ae7ad1a2cec8629f65b271d4b37c8c1024067c28fb7872da01bfabd9d65a8b",
    "8cd3637cfdff64bfa3c42c4a2080111e00ae505cc4a99dd25f0dc6d8d2d97924",
};

}  // namespace genko::test
