import urllib.request
import zipfile
import os
import shutil

print("1. Downloading HarfBuzz 8.2.1...")
urllib.request.urlretrieve("https://github.com/harfbuzz/harfbuzz/archive/refs/tags/8.2.1.zip", "hb.zip")

print("2. Extracting to lib/ folder...")
with zipfile.ZipFile("hb.zip", 'r') as zip_ref:
    zip_ref.extractall("lib/")

if os.path.exists("lib/harfbuzz"):
    shutil.rmtree("lib/harfbuzz")
os.rename("lib/harfbuzz-8.2.1", "lib/harfbuzz")

print("3. Configuring PlatformIO compiler rules...")
json_config = """{
  "name": "harfbuzz",
  "build": {
    "srcFilter": ["+<src/harfbuzz.cc>"],
    "flags": ["-Isrc", "-DHB_NO_MT", "-DHB_TINY", "-DHAVE_FREETYPE"]
  }
}"""
with open("lib/harfbuzz/library.json", "w") as f:
    f.write(json_config)

os.remove("hb.zip")
print("SUCCESS: HarfBuzz is ready for PlatformIO!")