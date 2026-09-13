const { defineConfig } = require ("@playwright/test");
const path = require ("node:path");

module.exports = defineConfig ({
  testDir: __dirname,
  testMatch: "*.spec.js",
  workers: 1,
  reporter: "list",
  use: {
    baseURL: "http://127.0.0.1:8003",
    browserName: "chromium",
    headless: true,
  },
  webServer: {
    command: "python3 scripts/package-site.py && python3 -m http.server 8003 --bind 127.0.0.1 --directory dist",
    cwd: path.resolve (__dirname, "../.."),
    url: "http://127.0.0.1:8003",
  },
});
