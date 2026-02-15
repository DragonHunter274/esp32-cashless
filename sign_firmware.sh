#!/bin/bash

# Firmware Signing Script for ESP32 OTA Updates
# This script signs the compiled firmware binary using the RSA private key
# The signature is embedded at the beginning of the firmware file
# Format: [512-byte signature][firmware binary]

set -e  # Exit on error

# Configuration
PRIV_KEY="priv_key.pem"
FIRMWARE_BIN=".pio/build/esp32-s3-devkitc-1/firmware.bin"
OUTPUT_DIR="firmware_release"
TEMP_SIGNATURE="firmware.sign"
OUTPUT_FILE="firmware.img"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "================================================"
echo "ESP32 Firmware Signing Tool"
echo "================================================"
echo ""

# Check if private key exists
if [ ! -f "$PRIV_KEY" ]; then
    echo -e "${RED}ERROR: Private key not found: $PRIV_KEY${NC}"
    echo "Please ensure priv_key.pem is in the project root directory"
    exit 1
fi

# Check if firmware binary exists
if [ ! -f "$FIRMWARE_BIN" ]; then
    echo -e "${RED}ERROR: Firmware binary not found: $FIRMWARE_BIN${NC}"
    echo "Please build the project first: pio run"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Get firmware version from command line or default
VERSION=${1:-"1.0.0"}

echo -e "${YELLOW}Firmware Version: $VERSION${NC}"
echo -e "${YELLOW}Input File: $FIRMWARE_BIN${NC}"
echo ""

# Generate binary signature (SHA256)
echo "Generating signature..."
openssl dgst -sign "$PRIV_KEY" -keyform PEM -sha256 -out "$OUTPUT_DIR/$TEMP_SIGNATURE" -binary "$FIRMWARE_BIN"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Signature generated successfully${NC}"
else
    echo -e "${RED}✗ Signature generation failed${NC}"
    exit 1
fi

# Get signature size
SIG_SIZE=$(stat -c%s "$OUTPUT_DIR/$TEMP_SIGNATURE")
echo "Signature size: $SIG_SIZE bytes"

# Verify the signature (sanity check)
echo "Verifying signature..."
openssl dgst -sha256 -verify rsa_key.pub -signature "$OUTPUT_DIR/$TEMP_SIGNATURE" "$FIRMWARE_BIN"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Signature verification successful${NC}"
else
    echo -e "${RED}✗ Signature verification failed${NC}"
    exit 1
fi

# Combine signature and firmware: [signature][firmware]
echo "Creating signed firmware image..."
cat "$OUTPUT_DIR/$TEMP_SIGNATURE" "$FIRMWARE_BIN" > "$OUTPUT_DIR/$OUTPUT_FILE"

# Clean up temporary signature file
rm "$OUTPUT_DIR/$TEMP_SIGNATURE"

echo -e "${GREEN}✓ Signed firmware image created${NC}"

# Generate manifest.json for esp32FOTA
echo "Generating manifest.json..."
cat > "$OUTPUT_DIR/manifest.json" << EOF
{
  "type": "esp32-fota-http",
  "version": "$VERSION",
  "bin": "$OUTPUT_FILE"
}
EOF

echo -e "${GREEN}✓ Manifest generated${NC}"

# Display file information
echo ""
echo "================================================"
echo "Signed Firmware Package Created"
echo "================================================"
echo "Output Directory: $OUTPUT_DIR/"
echo ""
echo "Files created:"
echo "  - $OUTPUT_FILE    ($(stat -c%s "$OUTPUT_DIR/$OUTPUT_FILE" | numfmt --to=iec-i --suffix=B))"
echo "    └─ Signature: $SIG_SIZE bytes"
echo "    └─ Firmware:  $(stat -c%s "$FIRMWARE_BIN" | numfmt --to=iec-i --suffix=B)"
echo "  - manifest.json"
echo ""
echo "Original firmware SHA256:"
sha256sum "$FIRMWARE_BIN" | awk '{print "  " $1}'
echo ""
echo "================================================"
echo "Next Steps:"
echo "================================================"
echo "1. Upload the contents of '$OUTPUT_DIR/' to your web server"
echo "2. Update the OTA_MANIFEST_URL in secrets.h to point to manifest.json"
echo "3. The ESP32 will check for updates every hour and apply them automatically"
echo ""
echo "Example server structure:"
echo "  http://yourserver.com/firmware/"
echo "    ├── manifest.json"
echo "    └── $OUTPUT_FILE (signature embedded)"
echo ""
