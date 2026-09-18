#!/bin/bash
# EMBODIOS UEFI Secure Boot Key Generation
# Generates certificates for UEFI Secure Boot: PK, KEK, and DB keys

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
SECUREBOOT_DIR="$KERNEL_DIR/secure-boot"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== EMBODIOS UEFI Key Generator ===${NC}"
echo ""

# Parse arguments
TEST_MODE=false
FORCE=false
VERBOSE=false
KEY_SIZE=2048
VALIDITY_DAYS=3650  # 10 years

while [[ $# -gt 0 ]]; do
    case $1 in
        --test)
            TEST_MODE=true
            shift
            ;;
        --force|-f)
            FORCE=true
            shift
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --key-size)
            KEY_SIZE="$2"
            shift 2
            ;;
        --validity)
            VALIDITY_DAYS="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Generates UEFI Secure Boot certificates (PK, KEK, DB)."
            echo ""
            echo "Options:"
            echo "  --test           Run in test mode with default values"
            echo "  --force          Overwrite existing keys"
            echo "  --verbose        Verbose output"
            echo "  --key-size SIZE  RSA key size (default: 2048)"
            echo "  --validity DAYS  Certificate validity in days (default: 3650)"
            echo "  --help           Show this help"
            echo ""
            echo "Generated files:"
            echo "  PK.key, PK.crt   - Platform Key (root of trust)"
            echo "  KEK.key, KEK.crt - Key Exchange Key (signs DB/DBX updates)"
            echo "  DB.key, DB.crt   - Signature Database (signs bootloaders)"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo -e "${RED}Error: $1 is required but not installed${NC}"
        return 1
    fi
    return 0
}

echo "Checking required tools..."
MISSING_TOOLS=0
check_tool "openssl" || MISSING_TOOLS=1

if [ $MISSING_TOOLS -eq 1 ]; then
    echo ""
    echo "Install missing tools:"
    echo "  macOS: brew install openssl"
    echo "  Ubuntu: sudo apt install openssl"
    exit 1
fi

# Check if keys already exist
if [ -d "$SECUREBOOT_DIR" ] && [ ! "$FORCE" = true ]; then
    if [ -f "$SECUREBOOT_DIR/DB.key" ] || [ -f "$SECUREBOOT_DIR/PK.key" ]; then
        echo -e "${YELLOW}Warning: Keys already exist in $SECUREBOOT_DIR${NC}"
        echo "Use --force to overwrite existing keys"
        exit 1
    fi
fi

# Create secure-boot directory
echo "Creating secure boot directory..."
mkdir -p "$SECUREBOOT_DIR"
chmod 700 "$SECUREBOOT_DIR"

# Generate certificate function
generate_cert() {
    local name=$1
    local common_name=$2
    local key_file="$SECUREBOOT_DIR/${name}.key"
    local crt_file="$SECUREBOOT_DIR/${name}.crt"

    if [ "$VERBOSE" = true ]; then
        echo -e "  ${BLUE}Generating ${name}...${NC}"
    fi

    # Generate private key
    openssl genrsa -out "$key_file" "$KEY_SIZE" 2>/dev/null

    # Set secure permissions on private key
    chmod 600 "$key_file"

    # Generate self-signed certificate
    openssl req -new -x509 \
        -key "$key_file" \
        -out "$crt_file" \
        -days "$VALIDITY_DAYS" \
        -sha256 \
        -subj "/CN=${common_name}/O=EMBODIOS/C=US" 2>/dev/null

    chmod 644 "$crt_file"

    if [ ! -f "$key_file" ] || [ ! -f "$crt_file" ]; then
        echo -e "${RED}Failed to generate ${name}${NC}"
        return 1
    fi

    return 0
}

# Generate Platform Key (PK)
echo "Generating Platform Key (PK)..."
if ! generate_cert "PK" "EMBODIOS Platform Key"; then
    exit 1
fi
echo -e "${GREEN}✓ PK generated${NC}"

# Generate Key Exchange Key (KEK)
echo "Generating Key Exchange Key (KEK)..."
if ! generate_cert "KEK" "EMBODIOS Key Exchange Key"; then
    exit 1
fi
echo -e "${GREEN}✓ KEK generated${NC}"

# Generate Signature Database Key (DB)
echo "Generating Signature Database Key (DB)..."
if ! generate_cert "DB" "EMBODIOS Signature Database Key"; then
    exit 1
fi
echo -e "${GREEN}✓ DB generated${NC}"

# Generate certificate info file
echo "Creating certificate info..."
cat > "$SECUREBOOT_DIR/CERTIFICATES.txt" << EOF
EMBODIOS UEFI Secure Boot Certificates
======================================

Generated: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Key Size: ${KEY_SIZE} bits
Validity: ${VALIDITY_DAYS} days (~$((VALIDITY_DAYS/365)) years)

Files:
------
PK.key, PK.crt   - Platform Key (root of trust)
KEK.key, KEK.crt - Key Exchange Key (signs DB/DBX updates)
DB.key, DB.crt   - Signature Database (authorizes bootloaders)

Usage:
------
1. Enroll PK, KEK, and DB certificates in UEFI firmware
2. Use DB.key to sign kernel: sbsign --key DB.key --cert DB.crt kernel.efi
3. Enable Secure Boot in UEFI settings

Security Notes:
---------------
- Keep .key files secure and private
- Back up keys to secure offline storage
- Consider hardware security module (HSM) for production
- Rotate certificates before expiry

Certificate Details:
--------------------
EOF

# Append certificate details
for cert in PK KEK DB; do
    echo "" >> "$SECUREBOOT_DIR/CERTIFICATES.txt"
    echo "${cert} Certificate:" >> "$SECUREBOOT_DIR/CERTIFICATES.txt"
    openssl x509 -in "$SECUREBOOT_DIR/${cert}.crt" -text -noout | \
        grep -A 2 "Subject:" >> "$SECUREBOOT_DIR/CERTIFICATES.txt" 2>/dev/null || true
done

# Verify generated certificates
echo ""
echo "Verifying certificates..."
VERIFY_FAILED=0

for cert in PK KEK DB; do
    key_file="$SECUREBOOT_DIR/${cert}.key"
    crt_file="$SECUREBOOT_DIR/${cert}.crt"

    # Check if files exist
    if [ ! -f "$key_file" ] || [ ! -f "$crt_file" ]; then
        echo -e "${RED}✗ ${cert} files missing${NC}"
        VERIFY_FAILED=1
        continue
    fi

    # Verify certificate is valid X.509
    if ! openssl x509 -in "$crt_file" -text -noout > /dev/null 2>&1; then
        echo -e "${RED}✗ ${cert} certificate invalid${NC}"
        VERIFY_FAILED=1
        continue
    fi

    # Verify key permissions
    key_perms=$(stat -f "%Lp" "$key_file" 2>/dev/null || stat -c "%a" "$key_file" 2>/dev/null)
    if [ "$key_perms" != "600" ]; then
        echo -e "${YELLOW}⚠ ${cert} key permissions not secure (${key_perms})${NC}"
    fi

    if [ "$VERBOSE" = true ]; then
        echo -e "${GREEN}✓ ${cert} valid${NC}"
    fi
done

if [ $VERIFY_FAILED -eq 1 ]; then
    echo -e "${RED}Verification failed${NC}"
    exit 1
fi

echo -e "${GREEN}✓ All certificates verified${NC}"

# Test mode output
if [ "$TEST_MODE" = true ]; then
    echo ""
    echo -e "${GREEN}=== Test Mode: Success ===${NC}"
    echo "Keys generated successfully"
fi

# Summary
echo ""
echo -e "${GREEN}=== Keys Generated Successfully ===${NC}"
echo "  Location: $SECUREBOOT_DIR"
echo "  Files:"
echo "    - PK.key, PK.crt   (Platform Key)"
echo "    - KEK.key, KEK.crt (Key Exchange Key)"
echo "    - DB.key, DB.crt   (Signature Database)"
echo ""
echo "Next steps:"
echo "  1. Review certificate info: cat $SECUREBOOT_DIR/CERTIFICATES.txt"
echo "  2. Sign kernel: ./scripts/sign_kernel.sh embodios.elf"
echo "  3. Enroll certificates in UEFI firmware"
echo ""
echo -e "${YELLOW}⚠ Important: Back up these keys to secure storage!${NC}"
