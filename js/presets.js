/* Language presets — add new entries here.
 * Each key becomes a preset button; the button label comes from
 * the HTML data-preset attribute, the text and font from here.
 *
 * Some presets ship with a small subset to keep the site small
 * but list a full font that the user can opt into downloading;
 * fullUrl / fullSize / fullName describe that upgrade. */
const PRESETS = {
  english:    { text: "hello-world!",      font: "fonts/NotoSans.ttf",            name: "NotoSans" },
  hebrew:     { text: "שלום עולם",          font: "fonts/NotoSansHebrew.ttf",     name: "NotoSansHebrew" },
  arabic:     { text: "مرحبا بالعالم",      font: "fonts/NotoNaskhArabic.ttf",    name: "NotoNaskhArabic" },
  urdu:       { text: "ہیلو دنیا",          font: "fonts/NotoNastaliqUrdu.ttf",   name: "NotoNastaliqUrdu" },
  hindi:      { text: "नमस्ते दुनिया",       font: "fonts/NotoSansDevanagari.ttf", name: "NotoSansDevanagari" },
  thai:       { text: "สวัสดีชาวโลก",       font: "fonts/NotoSansThaiLooped.ttf", name: "NotoSansThaiLooped" },
  khmer:      { text: "សួស្ដីពិភពលោក",      font: "fonts/NotoSansKhmer.ttf",      name: "NotoSansKhmer" },
  chinese:    { text: "你好世界！",          font: "fonts/NotoSansCJKsc-subset.otf", name: "NotoSansCJKsc",
                fullUrl: "https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c/google-fonts/NotoSansSC%5Bwght%5D.ttf",
                fullSize: 17772852, fullName: "NotoSansSC" },
  emoji:      { text: "🥰🦋👩🏻‍❤️‍👨🏼🌈🫠",   font: "fonts/NotoColorEmoji-subset.ttf",   name: "NotoColorEmoji",
                fullUrl: "https://raw.githubusercontent.com/googlefonts/noto-emoji/f3ae03f5e9b3b8516fa151f7168159ca1a3e7515/fonts/Noto-COLRv1.ttf",
                fullSize: 4991984, fullName: "NotoColorEmoji" },
};
