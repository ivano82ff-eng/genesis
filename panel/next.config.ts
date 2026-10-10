import type { NextConfig } from "next";
import path from "node:path";
import { fileURLToPath } from "node:url";

const panelRoot = path.dirname(fileURLToPath(import.meta.url));

const nextConfig: NextConfig = {
  allowedDevOrigins: ["127.0.0.1", "localhost"],
  turbopack: {
    root: panelRoot,
    rules: {
      "*.css": {
        loaders: ["@tailwindcss/turbopack"],
        as: "*.css",
      },
    },
  },
};

export default nextConfig;
