/* Language presets — add new entries here.
 * Each key becomes a preset button; the button label comes from
 * the HTML data-preset attribute, the text and font from here. */
const PRESETS = {
  english:    { text: "hello-world!",      font: "fonts/NotoSans.ttf",            name: "NotoSans" },
  hebrew:     { text: "שלום עולם",          font: "fonts/NotoSansHebrew.ttf",     name: "NotoSansHebrew" },
  arabic:     { text: "مرحبا بالعالم",      font: "fonts/NotoNaskhArabic.ttf",    name: "NotoNaskhArabic" },
  urdu:       { text: "ہیلو دنیا",          font: "fonts/NotoNastaliqUrdu.ttf",   name: "NotoNastaliqUrdu" },
  hindi:      { text: "नमस्ते दुनिया",       font: "fonts/NotoSansDevanagari.ttf", name: "NotoSansDevanagari" },
  thai:       { text: "สวัสดีชาวโลก",       font: "fonts/NotoSansThaiLooped.ttf", name: "NotoSansThaiLooped" },
  khmer:      { text: "សួស្ដីពិភពលោក",      font: "fonts/NotoSansKhmer.ttf",      name: "NotoSansKhmer" },
  chinese:    { text: "你好世界！",          font: "fonts/NotoSansCJKsc-subset.otf", name: "NotoSansCJKsc" },
  emoji:      { text: "🫠🌈❤️🦋🥰",         font: "fonts/NotoColorEmoji-subset.ttf",   name: "NotoColorEmoji" },
};
