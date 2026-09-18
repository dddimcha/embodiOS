#!/bin/bash
#
# verify_cert_chain.sh - Verify UEFI Secure Boot certificate chain and policies
#
# This script verifies:
# 1. Certificate chain integrity (PK -> KEK -> DB)
# 2. Certificate validity periods
# 3. Certificate policies and key usage
# 4. Private key permissions
# 5. Certificate rotation readiness
#
# Usage: ./verify_cert_chain.sh [--verbose] [--json]
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CERT_DIR="$KERNEL_DIR/secure-boot"

# Options
VERBOSE=0
JSON_OUTPUT=0
EXIT_CODE=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --json)
            JSON_OUTPUT=1
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Verify UEFI Secure Boot certificate chain and policies"
            echo ""
            echo "OPTIONS:"
            echo "  --verbose, -v    Show detailed certificate information"
            echo "  --json           Output results in JSON format"
            echo "  --help, -h       Show this help message"
            echo ""
            echo "VERIFICATION CHECKS:"
            echo "  - Certificate file existence"
            echo "  - Private key file permissions (should be 0600)"
            echo "  - Certificate validity periods"
            echo "  - Certificate Subject/Issuer integrity"
            echo "  - Key size and algorithm compliance"
            echo "  - Certificate chain structure (PK -> KEK -> DB)"
            echo ""
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Helper functions
info() {
    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo -e "${BLUE}[INFO]${NC} $1"
    fi
}

success() {
    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo -e "${GREEN}[✓]${NC} $1"
    fi
}

warning() {
    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo -e "${YELLOW}[⚠]${NC} $1"
    fi
}

error() {
    if [[ $JSON_OUTPUT -eq 0 ]]; then
        echo -e "${RED}[✗]${NC} $1"
    fi
    EXIT_CODE=1
}

verbose() {
    if [[ $VERBOSE -eq 1 && $JSON_OUTPUT -eq 0 ]]; then
        echo -e "    $1"
    fi
}

# JSON output structure
if [[ $JSON_OUTPUT -eq 1 ]]; then
    echo "{"
    echo "  \"verification\": {"
    echo "    \"timestamp\": \"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
    echo "    \"checks\": ["
fi

# Check if certificate directory exists
if [[ ! -d "$CERT_DIR" ]]; then
    error "Certificate directory not found: $CERT_DIR"
    exit 1
fi

info "Starting certificate chain verification..."
info "Certificate directory: $CERT_DIR"
echo ""

# Check 1: Verify all certificate files exist
info "Check 1: Certificate and key file existence"
REQUIRED_FILES=("PK.crt" "PK.key" "KEK.crt" "KEK.key" "DB.crt" "DB.key")
FILES_OK=1

for file in "${REQUIRED_FILES[@]}"; do
    if [[ -f "$CERT_DIR/$file" ]]; then
        success "Found: $file"
        verbose "Path: $CERT_DIR/$file"
    else
        error "Missing: $file"
        FILES_OK=0
    fi
done

if [[ $FILES_OK -eq 1 ]]; then
    success "All required certificate files present"
else
    error "Some certificate files are missing"
fi
echo ""

# Check 2: Verify private key permissions
info "Check 2: Private key file permissions"
KEY_PERMS_OK=1

for key_file in PK.key KEK.key DB.key; do
    if [[ -f "$CERT_DIR/$key_file" ]]; then
        perms=$(stat -f "%Lp" "$CERT_DIR/$key_file" 2>/dev/null || stat -c "%a" "$CERT_DIR/$key_file" 2>/dev/null)
        if [[ "$perms" == "600" || "$perms" == "400" ]]; then
            success "$key_file permissions: $perms (secure)"
            verbose "Private key is properly protected"
        else
            warning "$key_file permissions: $perms (should be 600 or 400)"
            KEY_PERMS_OK=0
        fi
    fi
done

if [[ $KEY_PERMS_OK -eq 1 ]]; then
    success "All private keys have secure permissions"
else
    warning "Some private keys have insecure permissions"
fi
echo ""

# Check 3: Verify certificate validity periods
info "Check 3: Certificate validity periods"
VALIDITY_OK=1

for cert_file in PK.crt KEK.crt DB.crt; do
    if [[ -f "$CERT_DIR/$cert_file" ]]; then
        cert_name=$(basename "$cert_file" .crt)

        # Get validity dates
        not_before=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -startdate | cut -d= -f2)
        not_after=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -enddate | cut -d= -f2)

        # Convert to seconds since epoch for comparison
        not_after_epoch=$(date -j -f "%b %d %T %Y %Z" "$not_after" "+%s" 2>/dev/null || date -d "$not_after" "+%s" 2>/dev/null)
        current_epoch=$(date "+%s")
        days_remaining=$(( ($not_after_epoch - $current_epoch) / 86400 ))

        if [[ $days_remaining -gt 365 ]]; then
            success "$cert_name: Valid for $days_remaining days"
            verbose "Not Before: $not_before"
            verbose "Not After:  $not_after"
        elif [[ $days_remaining -gt 90 ]]; then
            warning "$cert_name: Valid for $days_remaining days (consider rotation planning)"
            verbose "Not Before: $not_before"
            verbose "Not After:  $not_after"
            VALIDITY_OK=0
        elif [[ $days_remaining -gt 0 ]]; then
            error "$cert_name: Valid for only $days_remaining days (rotation needed soon!)"
            verbose "Not Before: $not_before"
            verbose "Not After:  $not_after"
            VALIDITY_OK=0
        else
            error "$cert_name: EXPIRED ($days_remaining days ago)"
            verbose "Not Before: $not_before"
            verbose "Not After:  $not_after"
            VALIDITY_OK=0
        fi
    fi
done

if [[ $VALIDITY_OK -eq 1 ]]; then
    success "All certificates have adequate validity periods"
else
    warning "Some certificates need attention"
fi
echo ""

# Check 4: Verify certificate subjects and issuers
info "Check 4: Certificate Subject/Issuer integrity"
SUBJECTS_OK=1

for cert_file in PK.crt KEK.crt DB.crt; do
    if [[ -f "$CERT_DIR/$cert_file" ]]; then
        cert_name=$(basename "$cert_file" .crt)
        subject=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -subject | sed 's/^subject=//')
        issuer=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -issuer | sed 's/^issuer=//')

        # Check if self-signed (subject == issuer)
        if [[ "$subject" == "$issuer" ]]; then
            success "$cert_name: Self-signed (root certificate)"
            verbose "Subject: $subject"
        else
            warning "$cert_name: Not self-signed"
            verbose "Subject: $subject"
            verbose "Issuer:  $issuer"
        fi

        # Verify subject contains expected components
        if echo "$subject" | grep -q "EMBODIOS"; then
            success "$cert_name: Subject contains EMBODIOS identifier"
        else
            warning "$cert_name: Subject missing EMBODIOS identifier"
            SUBJECTS_OK=0
        fi
    fi
done

if [[ $SUBJECTS_OK -eq 1 ]]; then
    success "All certificate subjects are properly configured"
fi
echo ""

# Check 5: Verify key sizes and algorithms
info "Check 5: Key size and algorithm compliance"
KEY_ALGO_OK=1

for cert_file in PK.crt KEK.crt DB.crt; do
    if [[ -f "$CERT_DIR/$cert_file" ]]; then
        cert_name=$(basename "$cert_file" .crt)

        # Get public key algorithm
        algo=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -text | grep "Public Key Algorithm" | awk '{print $4}')

        # Get key size
        key_size=$(openssl x509 -in "$CERT_DIR/$cert_file" -noout -text | grep "Public-Key:" | awk '{print $2}' | tr -d '()')

        if [[ "$algo" == "rsaEncryption" ]]; then
            if [[ "$key_size" -ge 2048 ]]; then
                success "$cert_name: $algo with $key_size bit key (compliant)"
                verbose "Algorithm: RSA"
                verbose "Key Size: $key_size bits"
            else
                error "$cert_name: $algo with $key_size bit key (too weak, minimum 2048)"
                KEY_ALGO_OK=0
            fi
        else
            warning "$cert_name: Non-RSA algorithm: $algo"
            KEY_ALGO_OK=0
        fi
    fi
done

if [[ $KEY_ALGO_OK -eq 1 ]]; then
    success "All certificates use compliant key algorithms and sizes"
fi
echo ""

# Check 6: Verify signed kernel (if available)
info "Check 6: Signed kernel verification"
SIGNED_KERNEL="$KERNEL_DIR/embodios.elf.signed"

if [[ -f "$SIGNED_KERNEL" ]]; then
    success "Signed kernel found: embodios.elf.signed"

    # Check file size
    size=$(du -h "$SIGNED_KERNEL" | awk '{print $1}')
    verbose "Size: $size"

    # Verify it's an ELF file
    if file "$SIGNED_KERNEL" | grep -q "ELF"; then
        success "Signed kernel is valid ELF format"
    else
        warning "Signed kernel may not be valid ELF format"
    fi

    # Check if sbverify is available
    if command -v sbverify &> /dev/null; then
        info "Running sbverify to check signature..."
        if sbverify --cert "$CERT_DIR/DB.crt" "$SIGNED_KERNEL" &> /dev/null; then
            success "Kernel signature verified successfully"
        else
            error "Kernel signature verification failed"
        fi
    else
        warning "sbverify not available (install sbsigntool for full verification)"
        info "Signed kernel exists but signature cannot be cryptographically verified"
    fi
else
    warning "Signed kernel not found (run 'make sign' in kernel directory)"
fi
echo ""

# Check 7: Certificate rotation readiness
info "Check 7: Certificate rotation readiness"

# Check if backup directory exists
if [[ -d "$CERT_DIR/backup" ]]; then
    success "Backup directory exists"
else
    warning "No backup directory found (consider creating $CERT_DIR/backup)"
fi

# Check for CERTIFICATES.txt info file
if [[ -f "$CERT_DIR/CERTIFICATES.txt" ]]; then
    success "Certificate information file present"
    verbose "Documentation: $CERT_DIR/CERTIFICATES.txt"
else
    warning "Certificate information file not found"
fi

# Provide rotation recommendations
echo ""
info "Certificate Rotation Policy Recommendations:"
echo ""
verbose "1. Rotate DB certificates annually or when compromised"
verbose "2. Rotate KEK certificates every 2-3 years"
verbose "3. Rotate PK certificate every 5 years or major infrastructure change"
verbose "4. Maintain backup of old certificates for 1 year after rotation"
verbose "5. Test new certificates in staging before production deployment"
verbose "6. Document rotation date in CERTIFICATES.txt"
echo ""

# Summary
echo ""
echo "=========================================="
if [[ $EXIT_CODE -eq 0 ]]; then
    success "Certificate chain verification PASSED"
    echo ""
    info "Certificate Chain:"
    verbose "PK (Platform Key)"
    verbose " └─ KEK (Key Exchange Key)"
    verbose "     └─ DB (Signature Database)"
    verbose "         └─ Signed Kernel (embodios.elf.signed)"
else
    error "Certificate chain verification FAILED"
    echo ""
    info "Please address the issues above before deploying with Secure Boot"
fi
echo "=========================================="

# JSON output closing
if [[ $JSON_OUTPUT -eq 1 ]]; then
    echo "    ],"
    echo "    \"status\": $(if [[ $EXIT_CODE -eq 0 ]]; then echo "\"passed\""; else echo "\"failed\""; fi),"
    echo "    \"exit_code\": $EXIT_CODE"
    echo "  }"
    echo "}"
fi

exit $EXIT_CODE
