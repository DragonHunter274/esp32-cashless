#!/usr/bin/env python3
"""
PlatformIO Post-Build Script
Automatically signs the firmware binary after compilation
"""

Import("env")
import os
import subprocess
import shutil
from pathlib import Path

def get_firmware_version():
    """Extract firmware version from OTA.h"""
    try:
        ota_header = Path("include/OTA.h")
        if ota_header.exists():
            with open(ota_header, 'r') as f:
                content = f.read()
                major = minor = patch = 0
                for line in content.split('\n'):
                    if 'FIRMWARE_VERSION_MAJOR' in line and '#define' in line:
                        major = line.split()[-1]
                    elif 'FIRMWARE_VERSION_MINOR' in line and '#define' in line:
                        minor = line.split()[-1]
                    elif 'FIRMWARE_VERSION_PATCH' in line and '#define' in line:
                        patch = line.split()[-1]
                return f"{major}.{minor}.{patch}"
    except Exception as e:
        print(f"Warning: Could not read version from OTA.h: {e}")
    return "1.0.0"

def sign_firmware(source, target, env):
    """Sign the firmware binary after build"""

    print("\n" + "="*60)
    print("POST-BUILD: Firmware Signing")
    print("="*60)

    # Paths
    project_dir = env.get("PROJECT_DIR")
    firmware_bin = str(target[0])
    priv_key = os.path.join(project_dir, "priv_key.pem")
    pub_key = os.path.join(project_dir, "rsa_key.pub")
    output_dir = os.path.join(project_dir, "firmware_release")
    temp_signature = os.path.join(output_dir, "firmware.sign")
    output_file = os.path.join(output_dir, "firmware.img")

    # Check if private key exists
    if not os.path.exists(priv_key):
        print(f"⚠ WARNING: Private key not found: {priv_key}")
        print("  Skipping firmware signing. To enable signing:")
        print("  1. Ensure priv_key.pem exists in project root")
        print("  2. Run: openssl genrsa -out priv_key.pem 4096")
        return

    # Check if public key exists
    if not os.path.exists(pub_key):
        print(f"⚠ WARNING: Public key not found: {pub_key}")
        print("  Skipping firmware signing.")
        return

    # Check if firmware binary exists
    if not os.path.exists(firmware_bin):
        print(f"❌ ERROR: Firmware binary not found: {firmware_bin}")
        return

    try:
        # Create output directory
        os.makedirs(output_dir, exist_ok=True)

        # Get firmware version
        version = get_firmware_version()
        print(f"📦 Firmware Version: {version}")
        print(f"📁 Input:  {firmware_bin}")
        print(f"📁 Output: {output_file}")
        print()

        # Get firmware size
        firmware_size = os.path.getsize(firmware_bin)
        print(f"Firmware size: {firmware_size:,} bytes ({firmware_size/1024:.2f} KB)")

        # Generate signature
        print("🔐 Generating RSA signature...")
        subprocess.run([
            "openssl", "dgst",
            "-sign", priv_key,
            "-keyform", "PEM",
            "-sha256",
            "-out", temp_signature,
            "-binary", firmware_bin
        ], check=True, capture_output=True)

        sig_size = os.path.getsize(temp_signature)
        print(f"✓ Signature generated: {sig_size} bytes")

        # Verify signature (sanity check)
        print("🔍 Verifying signature...")
        result = subprocess.run([
            "openssl", "dgst",
            "-sha256",
            "-verify", pub_key,
            "-signature", temp_signature,
            firmware_bin
        ], capture_output=True, text=True)

        if result.returncode == 0:
            print("✓ Signature verification successful")
        else:
            print(f"❌ Signature verification failed: {result.stderr}")
            os.remove(temp_signature)
            return

        # Combine signature + firmware
        print("📝 Creating signed firmware image...")
        with open(output_file, 'wb') as outf:
            with open(temp_signature, 'rb') as sigf:
                outf.write(sigf.read())
            with open(firmware_bin, 'rb') as binf:
                outf.write(binf.read())

        # Clean up temp signature
        os.remove(temp_signature)

        final_size = os.path.getsize(output_file)
        print(f"✓ Signed firmware created: {final_size:,} bytes")

        # Generate manifest.json
        print("📋 Generating manifest.json...")
        manifest_content = f'''{{
  "type": "esp32-fota-http",
  "version": "{version}",
  "bin": "firmware.img"
}}
'''
        manifest_file = os.path.join(output_dir, "manifest.json")
        with open(manifest_file, 'w') as f:
            f.write(manifest_content)

        print("✓ Manifest generated")

        # Summary
        print()
        print("="*60)
        print("✅ FIRMWARE SIGNING COMPLETE")
        print("="*60)
        print(f"Output directory: {output_dir}/")
        print(f"  - firmware.img    ({final_size:,} bytes)")
        print(f"    └─ Signature:   {sig_size} bytes")
        print(f"    └─ Firmware:    {firmware_size:,} bytes")
        print(f"  - manifest.json")
        print()
        print("Next steps:")
        print(f"  1. Upload {output_dir}/ contents to your web server")
        print(f"  2. ESP32 will auto-update to version {version}")
        print("="*60 + "\n")

    except subprocess.CalledProcessError as e:
        print(f"❌ ERROR during signing: {e}")
        print(f"   Output: {e.output}")
        if os.path.exists(temp_signature):
            os.remove(temp_signature)
    except Exception as e:
        print(f"❌ ERROR: {e}")
        if os.path.exists(temp_signature):
            os.remove(temp_signature)

# Register the post-build action
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign_firmware)
