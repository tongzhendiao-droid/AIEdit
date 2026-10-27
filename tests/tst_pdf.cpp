#include "core/Pdf.h"

#include <QByteArray>
#include <QTest>

using aiedit::core::extractPdfText;
using aiedit::core::PdfText;

namespace {

// Two pages: a WinAnsi font with TJ spacing and escapes; a Type0 font with a ToUnicode map
// (bfchar and bfrange); /Differences with glyph names; a form XObject; an inline image; and on
// the second page, content in two streams. Written by hand (see the commit adding it).
const char kFonts[] =
    "JVBERi0xLjUKJeLjz9MKMSAwIG9iago8PCAvVHlwZSAvQ2F0YWxvZyAvUGFnZXMgMiAwIFIgPj4KZW5kb2JqCjIgMCBvYmoKPDwg"
    "L1R5cGUgL1BhZ2VzIC9LaWRzIFszIDAgUiA5IDAgUl0gL0NvdW50IDIgL1Jlc291cmNlcyA8PCAvRm9udCA8PCAvRjEgNSAwIFIg"
    "L0YyIDYgMCBSIC9GMyA4IDAgUiA+PiAvWE9iamVjdCA8PCAvRm0xIDEwIDAgUiA+PiA+PiA+PgplbmRvYmoKMyAwIG9iago8PCAv"
    "VHlwZSAvUGFnZSAvUGFyZW50IDIgMCBSIC9Db250ZW50cyA0IDAgUiA+PgplbmRvYmoKNCAwIG9iago8PCAvTGVuZ3RoIDE2OSAv"
    "RmlsdGVyIC9GbGF0ZURlY29kZSA+PgpzdHJlYW0KeJxFTrsOgkAQ1JavmPKuILd7qFgQC16ClYmXUIgFETEalMTKv9fjkVDMJrvz"
    "2tBApQzWMA18DZ8IpobIbm3bSZgnzqKQrEl0n7aW7oZIVPfq8ZYXmINDcPVouFZN6a0ZpfiWsjfOnEr1VBAQEVvoXZ8cjAvbwcPF"
    "Uak3Kefg0rIWengmMVbzYsSdE+ZQBVZQGRgqPEbYQkUnqD3yGIsk/y2R5M4fdcAzpAplbmRzdHJlYW0KZW5kb2JqCjUgMCBvYmoK"
    "PDwgL1R5cGUgL0ZvbnQgL1N1YnR5cGUgL1R5cGUxIC9CYXNlRm9udCAvSGVsdmV0aWNhIC9FbmNvZGluZyAvV2luQW5zaUVuY29k"
    "aW5nID4+CmVuZG9iago2IDAgb2JqCjw8IC9UeXBlIC9Gb250IC9TdWJ0eXBlIC9UeXBlMCAvQmFzZUZvbnQgL1ggL0VuY29kaW5n"
    "IC9JZGVudGl0eS1IIC9EZXNjZW5kYW50Rm9udHMgWzExIDAgUl0gL1RvVW5pY29kZSA3IDAgUiA+PgplbmRvYmoKNyAwIG9iago8"
    "PCAvTGVuZ3RoIDIyOSA+PgpzdHJlYW0KL0NJREluaXQgL1Byb2NTZXQgZmluZHJlc291cmNlIGJlZ2luIDEyIGRpY3QgYmVnaW4g"
    "YmVnaW5jbWFwCjEgYmVnaW5jb2Rlc3BhY2VyYW5nZSA8MDAwMD4gPEZGRkY+IGVuZGNvZGVzcGFjZXJhbmdlCjIgYmVnaW5iZmNo"
    "YXIgPDAwMDE+IDw0RjYwPiA8MDAwMj4gPDU5N0Q+IGVuZGJmY2hhcgoxIGJlZ2luYmZyYW5nZSA8MDAxMD4gPDAwMTI+IDwwMDQx"
    "PiBlbmRiZnJhbmdlCmVuZGNtYXAgZW5kIGVuZAplbmRzdHJlYW0KZW5kb2JqCjggMCBvYmoKPDwgL1R5cGUgL0ZvbnQgL1N1YnR5"
    "cGUgL1R5cGUxIC9CYXNlRm9udCAvWSAvRW5jb2RpbmcgPDwgL0RpZmZlcmVuY2VzIFsxIC9maSAvcXVvdGVyaWdodF0gPj4gPj4K"
    "ZW5kb2JqCjkgMCBvYmoKPDwgL1R5cGUgL1BhZ2UgL1BhcmVudCAyIDAgUiAvQ29udGVudHMgWzEyIDAgUiAxMyAwIFJdID4+CmVu"
    "ZG9iagoxMCAwIG9iago8PCAvTGVuZ3RoIDQwIC9UeXBlIC9YT2JqZWN0IC9TdWJ0eXBlIC9Gb3JtIC9CQm94IFswIDAgNjAwIDgw"
    "MF0gPj4Kc3RyZWFtCkJUIC9GMSAxMCBUZiA3MiA1MDAgVGQgKEluIGEgZm9ybSkgVGogRVQKZW5kc3RyZWFtCmVuZG9iagoxMSAw"
    "IG9iago8PCAvVHlwZSAvRm9udCAvU3VidHlwZSAvQ0lERm9udFR5cGUyIC9CYXNlRm9udCAvWCAvRFcgMTAwMCA+PgplbmRvYmoK"
    "MTIgMCBvYmoKPDwgL0xlbmd0aCAyMiA+PgpzdHJlYW0KQlQgL0YxIDEyIFRmIDcyIDcwMCBUZAplbmRzdHJlYW0KZW5kb2JqCjEz"
    "IDAgb2JqCjw8IC9MZW5ndGggMTkgPj4Kc3RyZWFtCihTZWNvbmQgcGFnZSkgVGogRVQKZW5kc3RyZWFtCmVuZG9iagp0cmFpbGVy"
    "Cjw8IC9Sb290IDEgMCBSID4+CiUlRU9GCg==";

// Its objects packed in a compressed object stream, the content in ASCIIHex, a cross-reference
// stream instead of a trailer.
const char kPacked[] =
    "JVBERi0xLjUKJeLjz9MKNiAwIG9iago8PCAvTGVuZ3RoIDE1MyAvVHlwZSAvT2JqU3RtIC9OIDQgL0ZpcnN0IDIwIC9GaWx0ZXIg"
    "L0ZsYXRlRGVjb2RlID4+CnN0cmVhbQp4nGWOwQrCMBBEf2X+YBtT66X0oCAFL1K9iYdYl1IojTSp4N+76QoKQgg7eTOzMciwgs1h"
    "sSmwhiksyhJ0fj0YtHPRDb4DHV3HQYwZGlTVj0MBHfp7wMUmfpWYn8cI8+9M98TCtEmMYxQZkKtuOPh5aqUx5fZCdTDys89qPd/a"
    "xUSn+RYXmR4NaOsCK6l5eHLsW5dib2LBQfMKZW5kc3RyZWFtCmVuZG9iago0IDAgb2JqCjw8IC9MZW5ndGggNzUgL0ZpbHRlciAv"
    "QVNDSUlIZXhEZWNvZGUgPj4Kc3RyZWFtCjQyNTQyMDJmNDYzMTIwMzEzMjIwNTQ2NjIwMzczMjIwMzczMDMwMjA1NDY0MjAyODUw"
    "NjE2MzZiNjU2NDI5MjA1NDZhMjA0NTU0PgplbmRzdHJlYW0KZW5kb2JqCjcgMCBvYmoKPDwgL0xlbmd0aCAwIC9UeXBlIC9YUmVm"
    "IC9Sb290IDEgMCBSIC9TaXplIDggPj4Kc3RyZWFtCgplbmRzdHJlYW0KZW5kb2JqCiUlRU9GCg==";

QByteArray bytes(const char* base64)
{
    return QByteArray::fromBase64(base64);
}

PdfText extract(const QByteArray& pdf, std::size_t maxText = 1 << 20)
{
    return extractPdfText(std::span(reinterpret_cast<const std::uint8_t*>(pdf.constData()),
                                    static_cast<std::size_t>(pdf.size())),
                          maxText);
}

QString text(const PdfText& pdf)
{
    return QString::fromUtf8(pdf.utf8.data(), static_cast<qsizetype>(pdf.utf8.size()));
}

} // namespace

class TestPdf : public QObject
{
    Q_OBJECT

private slots:
    void fontsAndLayout();
    void objectStreams();
    void encrypted();
    void truncated();
    void notPdf();
};

void TestPdf::fontsAndLayout()
{
    const PdfText pdf = extract(bytes(kFonts));
    QCOMPARE(pdf.status, PdfText::Status::Ok);
    QCOMPARE(pdf.pages, 2);
    QCOMPARE(pdf.unmappedGlyphs, 0u);
    QCOMPARE(text(pdf), QStringLiteral("HelloWorld again\n"
                                       "café (x)\n"
                                       "你好ABC\n"
                                       "ﬁ’\n"
                                       "In a form\n\n"
                                       "Second page\n"));
}

void TestPdf::objectStreams()
{
    const PdfText pdf = extract(bytes(kPacked));
    QCOMPARE(pdf.status, PdfText::Status::Ok);
    QCOMPARE(text(pdf), QStringLiteral("Packed\n"));
}

void TestPdf::encrypted()
{
    QByteArray pdf = bytes(kFonts);
    pdf.replace("<< /Root 1 0 R >>", "<< /Root 1 0 R /Encrypt << /Filter /Standard /V 1 /R 2 >> >>");
    const PdfText result = extract(pdf);
    QCOMPARE(result.status, PdfText::Status::Encrypted);
    QVERIFY(result.utf8.empty());
}

void TestPdf::truncated()
{
    const PdfText pdf = extract(bytes(kFonts), 12);
    QCOMPARE(pdf.status, PdfText::Status::Truncated);
    QVERIFY(pdf.utf8.size() <= 12);
    QVERIFY(text(pdf).startsWith(QStringLiteral("HelloWorld")));
}

void TestPdf::notPdf()
{
    QCOMPARE(extract("just some text").status, PdfText::Status::NoText);
    QCOMPARE(extract(QByteArray()).status, PdfText::Status::NoText);
    // A hex string left open at the end of the file once read one byte past it.
    QCOMPARE(extract("%PDF-1.4\n1 0 obj <</Length 3>> <4142").status, PdfText::Status::NoText);
    // Cut in half: whatever can still be read, without crashing.
    const QByteArray pdf = bytes(kFonts);
    for (qsizetype size = 0; size < pdf.size(); size += 37)
        extract(pdf.left(size));
}

QTEST_GUILESS_MAIN(TestPdf)
#include "tst_pdf.moc"
