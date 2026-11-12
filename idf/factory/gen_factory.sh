#!/usr/bin/env bash
# Generates a unique Matter factory partition (fctry) for one device:
# random passcode + discriminator, a new DAC signed by the public Matter
# test PAI (VID 0xFFF1 / PID 0x8000), plus vendor/product names.
#
# Output: factory/out/fff1_8000/<uuid>/
#   <uuid>-partition.bin   -> flash at the fctry offset (see partitions.csv)
#   <uuid>-onb_codes.csv   -> QR payload + manual pairing code
#   <uuid>-qrcode.png      -> printable QR
#
# The output contains the device's DAC private key and passcode: keep it out of git.
#
# Usage: source the IDF env first, then  ./factory/gen_factory.sh
set -euo pipefail
cd "$(dirname "$0")"

CREDS=test-creds
esp-matter-mfg-tool \
    --target esp32c6 \
    -v 0xFFF1 -p 0x8000 \
    --vendor-name "Ismael Carrea" \
    --product-name "Air Quality Sensor V1" \
    --hw-ver 1 --hw-ver-str "v1" \
    --mfg-date "$(date +%F)" \
    --serial-num "AQS1-$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' \n' | tr a-f A-F)" \
    --enable-rotating-device-id \
    --pai -c "$CREDS/Chip-Test-PAI-FFF1-8000-Cert.pem" -k "$CREDS/Chip-Test-PAI-FFF1-8000-Key.pem" \
    -cd "$CREDS/Chip-Test-CD-FFF1-8000.der" \
    -cf 0 -dm 2 \
    -s 0x6000 \
    --outdir out

# The factory data only holds the SPAKE2+ verifier, but this device draws its
# own QR code on the e-paper, so it also needs the passcode: add it as
# chip-factory/pin-code and rebuild the partition with IDF's NVS generator.
latest=$(ls -td out/fff1_8000/*-*-*-*-*/ | head -1)
passcode=$(tail -1 "$latest"/*-onb_codes.csv | cut -d, -f4)
cp "$latest/internal/partition.csv" out/fctry.csv
echo "pin-code,data,u32,$passcode" >> out/fctry.csv
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
    generate out/fctry.csv out/fctry.bin 0x6000

# Stable names for the build (main/CMakeLists.txt flashes out/fctry.bin with `idf.py flash`)
cp "$latest"/*-onb_codes.csv out/onb_codes.csv
cp "$latest"/*-qrcode.png out/qrcode.png
echo "Factory partition: factory/out/fctry.bin"
cat out/onb_codes.csv
