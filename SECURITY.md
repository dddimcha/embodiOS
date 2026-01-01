# Security Policy

## Supported Versions

EMBODIOS is currently in active development. Security updates are applied to the latest version on the `main` branch.

| Version | Supported          |
| ------- | ------------------ |
| main    | :white_check_mark: |
| < main  | :x:                |

## Reporting a Vulnerability

We take the security of EMBODIOS seriously. If you believe you have found a security vulnerability, please report it to us as described below.

### How to Report

**Please do NOT report security vulnerabilities through public GitHub issues.**

Instead, please report them via one of these methods:

1. **GitHub Security Advisories**: Use the "Report a vulnerability" button in the Security tab of this repository
2. **Private Issue**: Create an issue with `[SECURITY]` prefix and request it be made private

### What to Include

Please include the following information in your report:

- Type of issue (e.g., buffer overflow, privilege escalation, memory corruption)
- Full paths of source file(s) related to the issue
- Location of the affected source code (tag/branch/commit or direct URL)
- Step-by-step instructions to reproduce the issue
- Proof-of-concept or exploit code (if possible)
- Impact of the issue, including how an attacker might exploit it

### Response Timeline

- **Initial Response**: Within 48 hours
- **Status Update**: Within 7 days
- **Resolution Target**: Within 30 days for critical issues

### What to Expect

1. We will acknowledge your report within 48 hours
2. We will provide a more detailed response within 7 days
3. We will work with you to understand and resolve the issue
4. We will keep you informed of our progress
5. We will credit you in the security advisory (unless you prefer to remain anonymous)

## Security Considerations for EMBODIOS

As a bare-metal operating system running AI models in kernel space, EMBODIOS has unique security considerations:

### Known Limitations (Development Phase)

- **No memory protection**: Currently runs in flat memory model
- **No privilege separation**: All code runs in kernel mode
- **No secure boot**: Boot chain is not cryptographically verified

### Planned Security Features

- Memory protection via MPU/MMU
- Secure boot support
- Model integrity verification
- Hardware security module (HSM) integration

## Disclosure Policy

We follow a coordinated disclosure process:

1. Reporter submits vulnerability
2. We confirm and assess severity
3. We develop and test a fix
4. We release the fix
5. We publish a security advisory
6. Reporter may publish their findings after advisory is public

Thank you for helping keep EMBODIOS and its users safe!
