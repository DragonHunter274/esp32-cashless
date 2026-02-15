#!/bin/bash

# Firmware Verification Script
# Verifies a signed firmware.img file by extracting and checking the embedded signature

set -e

# Configuration
FIRMWARE_IMG=${1:-"firmware_release/firmware.img"}
PUB_KEY="rsa_key.pub"
TEMP_DIR=$(mktemp -d)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "================================================"
echo "Firmware Signature Verification Tool"
echo "================================================"
echo ""

# Check if firmware file exists
if [ ! -f "$FIRMWARE_IMG" ]; then
    echo -e "${RED}ERROR: Firmware file not found: $FIRMWARE_IMG${NC}"
    echo "Usage: ./verify_firmware.sh [path/to/firmware.img]"
    exit 1
fi

# Check if public key exists
if [ ! -f "$PUB_KEY" ]; then
    echo -e "${RED}ERROR: Public key not found: $PUB_KEY${NC}"
    exit 1
fi

echo -e "${YELLOW}Firmware: $FIRMWARE_IMG${NC}"
echo -e "${YELLOW}Public Key: $PUB_KEY${NC}"
echo ""

# Get file size
FILE_SIZE=$(stat -c%s "$FIRMWARE_IMG")
echo "Total file size: $(numfmt --to=iec-i --suffix=B $FILE_SIZE)"

# Extract signature (first 512 bytes)
echo "Extracting signature..."
dd if="$FIRMWARE_IMG" of="$TEMP_DIR/signature.bin" bs=1 count=512 2>/dev/null

SIG_SIZE=$(stat -c%s "$TEMP_DIR/signature.bin")
echo "Signature size: $SIG_SIZE bytes"

# Extract firmware (everything after signature)
echo "Extracting firmware..."
dd if="$FIRMWARE_IMG" of="$TEMP_DIR/firmware.bin" bs=1 skip=512 2>/dev/null

FW_SIZE=$(stat -c%s "$TEMP_DIR/firmware.bin")
echo "Firmware size: $(numfmt --to=iec-i --suffix=B $FW_SIZE)"
echo ""

# Verify signature
echo "Verifying signature..."
if openssl dgst -sha256 -verify "$PUB_KEY" -signature "$TEMP_DIR/signature.bin" "$TEMP_DIR/firmware.bin" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ Signature verification PASSED${NC}"
    echo -e "${GREEN}✓ Firmware is authentic and unmodified${NC}"
    RESULT=0
else
    echo -e "${RED}✗ Signature verification FAILED${NC}"
    echo -e "${RED}✗ Firmware may be corrupted or tampered with${NC}"
    RESULT=1
fi

echo ""
echo "Firmware SHA256:"
sha256sum "$TEMP_DIR/firmware.bin" | awk '{print "  " $1}'

# Cleanup
rm -rf "$TEMP_DIR"

echo ""
exit $RESULT
