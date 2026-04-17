#!/bin/sh
# Chi Programming Language Installer
#
# Usage:
#   curl -fsSL https://haiyami.com/chi/install.sh | sh
#   curl -fsSL https://haiyami.com/chi/install.sh | sh -s -- --version v0.9.0
#
# Environment variables:
#   CHI_INSTALL_DIR    Override install location (default: ~/.chi)
#   CHI_DOWNLOAD_URL   Override base download URL (for mirrors/testing)
#   CHI_LIBC           Override Linux libc detection: "gnu" or "musl"
#   GITHUB_TOKEN       GitHub API token for private repos or rate limiting

set -u

GITHUB_REPO="haiyamicode/chi"
INSTALL_DIR="${CHI_INSTALL_DIR:-}"
VERSION=""
LIBC="${CHI_LIBC:-}"
TMPDIR_CLEANUP=""

# --- Output helpers ---

BOLD=""
RESET=""
RED=""
GREEN=""
YELLOW=""

if [ -t 2 ]; then
    BOLD="$(printf '\033[1m')"
    RESET="$(printf '\033[0m')"
    RED="$(printf '\033[31m')"
    GREEN="$(printf '\033[32m')"
    YELLOW="$(printf '\033[33m')"
fi

info() {
    printf '%s\n' "$@"
}

warn() {
    printf '%s%swarn%s: %s\n' "$YELLOW" "$BOLD" "$RESET" "$*" >&2
}

err() {
    printf '%s%serror%s: %s\n' "$RED" "$BOLD" "$RESET" "$*" >&2
}

# --- Prerequisites ---

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        err "required command '$1' not found"
        exit 1
    fi
}

check_cmd() {
    command -v "$1" >/dev/null 2>&1
}

# --- Platform detection ---

detect_platform() {
    local os arch

    os=$(uname -s)
    arch=$(uname -m)

    case "$os" in
        Darwin) os="macos" ;;
        Linux)  os="linux" ;;
        *)
            err "unsupported operating system: $os"
            err "chi currently supports macOS and Linux"
            exit 1
            ;;
    esac

    # Rosetta detection: uname -m lies under Rosetta 2
    if [ "$os" = "macos" ] && [ "$arch" = "x86_64" ]; then
        if sysctl -n sysctl.proc_translated 2>/dev/null | grep -q 1; then
            arch="arm64"
        fi
    fi

    case "$arch" in
        x86_64|amd64)   arch="x86_64" ;;
        aarch64|arm64)   arch="arm64" ;;
        *)
            err "unsupported architecture: $arch"
            err "chi currently supports x86_64 and arm64"
            exit 1
            ;;
    esac

    PLATFORM="${os}-${arch}"
    if [ "$os" = "linux" ]; then
        detect_libc
        PLATFORM="${PLATFORM}-${LIBC}"
    fi
}

# Detect glibc vs musl. Rustup-style ldd probe: glibc's `ldd --version` prints
# "ldd (GNU libc) ..."; musl's prints "musl libc ..." on stderr with exit 1.
# `2>&1` captures both, grep matches "musl" case-insensitively.
detect_libc() {
    if [ -n "$LIBC" ]; then
        case "$LIBC" in
            gnu|musl) ;;
            *)
                err "invalid CHI_LIBC value: '$LIBC' (must be 'gnu' or 'musl')"
                exit 1
                ;;
        esac
        return
    fi

    if check_cmd ldd; then
        if ldd --version 2>&1 | grep -qi musl; then
            LIBC="musl"
            return
        fi
        LIBC="gnu"
        return
    fi

    # Fallback: check for musl's dynamic linker. If neither ldd nor a musl
    # linker is present, default to gnu (the common case) with a warning.
    if [ -f /lib/ld-musl-x86_64.so.1 ] || [ -f /lib/ld-musl-aarch64.so.1 ]; then
        LIBC="musl"
        return
    fi

    warn "could not detect libc (ldd missing); defaulting to 'gnu'"
    warn "override with CHI_LIBC=musl if this is a musl system"
    LIBC="gnu"
}

# --- Version detection ---

detect_version() {
    if [ -n "$VERSION" ]; then
        return
    fi

    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    local auth_header=""
    if [ -n "${GITHUB_TOKEN:-}" ]; then
        auth_header="Authorization: token ${GITHUB_TOKEN}"
    fi

    local response=""
    if check_cmd curl; then
        if [ -n "$auth_header" ]; then
            response=$(curl -fsSL -H "$auth_header" "$api_url" 2>/dev/null) || true
        else
            response=$(curl -fsSL "$api_url" 2>/dev/null) || true
        fi
    elif check_cmd wget; then
        if [ -n "$auth_header" ]; then
            response=$(wget -qO- --header="$auth_header" "$api_url" 2>/dev/null) || true
        else
            response=$(wget -qO- "$api_url" 2>/dev/null) || true
        fi
    fi

    VERSION=$(printf '%s' "$response" | grep '"tag_name"' | sed 's/.*: "//;s/".*//')

    if [ -z "$VERSION" ]; then
        err "could not determine latest version"
        if printf '%s' "$response" | grep -q "rate limit" 2>/dev/null; then
            err "GitHub API rate limit exceeded — try setting GITHUB_TOKEN"
        fi
        err "you can specify a version manually:"
        err "  curl -fsSL <url> | sh -s -- --version v0.9.0"
        exit 1
    fi
}

# --- Download ---

download() {
    local url="$1"
    local output="$2"

    if check_cmd curl; then
        if [ -n "${GITHUB_TOKEN:-}" ]; then
            curl --fail --location --progress-bar \
                -H "Authorization: token ${GITHUB_TOKEN}" \
                -H "Accept: application/octet-stream" \
                --output "$output" "$url"
        else
            curl --fail --location --progress-bar --output "$output" "$url"
        fi
    elif check_cmd wget; then
        if [ -n "${GITHUB_TOKEN:-}" ]; then
            wget --quiet --show-progress \
                --header="Authorization: token ${GITHUB_TOKEN}" \
                --header="Accept: application/octet-stream" \
                --output-document="$output" "$url"
        else
            wget --quiet --show-progress --output-document="$output" "$url"
        fi
    fi
}

# --- Shell configuration ---

configure_shell() {
    local shell_name
    shell_name=$(basename "${SHELL:-/bin/sh}")

    # Write env script (sourced from rcfiles)
    cat > "${INSTALL_DIR}/env" <<'ENVEOF'
# Chi language environment — added by chi installer
case ":${PATH}:" in
    *:"$HOME/.chi/bin":*) ;;
    *) export PATH="$HOME/.chi/bin:$PATH" ;;
esac
ENVEOF

    local source_line=". \"\$HOME/.chi/env\""
    local modified=""

    case "$shell_name" in
        zsh)
            local rcfile="${ZDOTDIR:-$HOME}/.zshrc"
            if ! grep -q '.chi/env' "$rcfile" 2>/dev/null; then
                printf '\n# Chi language\n%s\n' "$source_line" >> "$rcfile"
                modified="$rcfile"
            fi
            ;;
        bash)
            # On macOS, bash reads .bash_profile for login shells (Terminal.app)
            local rcfile="$HOME/.bashrc"
            if [ "$(uname -s)" = "Darwin" ] && [ -f "$HOME/.bash_profile" ]; then
                rcfile="$HOME/.bash_profile"
            fi
            if ! grep -q '.chi/env' "$rcfile" 2>/dev/null; then
                printf '\n# Chi language\n%s\n' "$source_line" >> "$rcfile"
                modified="$rcfile"
            fi
            ;;
        fish)
            local fish_dir="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d"
            mkdir -p "$fish_dir"
            cat > "$fish_dir/chi.fish" <<FISHEOF
# Chi language environment — added by chi installer
if not contains "\$HOME/.chi/bin" \$PATH
    fish_add_path "$HOME/.chi/bin"
end
FISHEOF
            modified="$fish_dir/chi.fish"
            ;;
        *)
            # Fallback: .profile
            if ! grep -q '.chi/env' "$HOME/.profile" 2>/dev/null; then
                printf '\n# Chi language\n%s\n' "$source_line" >> "$HOME/.profile"
                modified="$HOME/.profile"
            fi
            ;;
    esac

    MODIFIED_FILES="${modified:-}"
}

# --- Main ---

main() {
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --version|-v) VERSION="$2"; shift 2 ;;
            --libc)
                LIBC="$2"
                case "$LIBC" in
                    gnu|musl) ;;
                    *)
                        err "invalid --libc value: '$LIBC' (must be 'gnu' or 'musl')"
                        exit 1
                        ;;
                esac
                shift 2
                ;;
            --help|-h)
                info "Chi Programming Language Installer"
                info ""
                info "Usage: curl -fsSL <url> | sh"
                info "       curl -fsSL <url> | sh -s -- [options]"
                info ""
                info "Options:"
                info "  --version, -v <version>   Install a specific version (e.g., v0.9.0)"
                info "  --libc <gnu|musl>         Linux libc variant (auto-detected)"
                info "  --help, -h                Show this help"
                info ""
                info "Environment variables:"
                info "  CHI_INSTALL_DIR    Override install location (default: ~/.chi)"
                info "  CHI_DOWNLOAD_URL   Override base download URL"
                info "  CHI_LIBC           Override Linux libc detection (gnu or musl)"
                info "  GITHUB_TOKEN       GitHub API token for private repos"
                exit 0
                ;;
            *)
                err "unknown option: $1"
                err "run with --help for usage"
                exit 1
                ;;
        esac
    done

    # Resolve install directory
    if [ -z "$INSTALL_DIR" ]; then
        if [ -z "${HOME:-}" ]; then
            err "\$HOME is not set — cannot determine install directory"
            err "set CHI_INSTALL_DIR to specify where to install"
            exit 1
        fi
        INSTALL_DIR="$HOME/.chi"
    fi

    # Check prerequisites
    need_cmd uname
    need_cmd tar
    need_cmd mktemp
    need_cmd chmod
    need_cmd mkdir
    need_cmd rm
    need_cmd grep
    need_cmd sed

    if ! check_cmd curl && ! check_cmd wget; then
        err "either 'curl' or 'wget' is required to download chi"
        exit 1
    fi

    detect_platform
    detect_version

    info ""
    info "  ${BOLD}Chi ${VERSION}${RESET}"
    info "  Platform:  ${PLATFORM}"
    info "  Location:  ${INSTALL_DIR}"
    if [ -n "${LIBC:-}" ] && [ -n "${CHI_LIBC:-}" ]; then
        info "  Libc:      ${LIBC} (override)"
    fi
    info ""

    # Construct download URL
    local archive="chi-${VERSION}-${PLATFORM}.tar.gz"
    local base_url="${CHI_DOWNLOAD_URL:-https://github.com/${GITHUB_REPO}/releases/download/${VERSION}}"
    local url="${base_url}/${archive}"

    # Download to temp directory
    TMPDIR_CLEANUP=$(mktemp -d) || { err "failed to create temp directory"; exit 1; }
    trap 'rm -rf "$TMPDIR_CLEANUP"' EXIT
    local tmpdir="$TMPDIR_CLEANUP"

    if ! download "$url" "${tmpdir}/${archive}"; then
        err "download failed: ${url}"
        err ""
        err "possible causes:"
        err "  - version '${VERSION}' does not exist"
        err "  - no release artifact for '${PLATFORM}'"
        err "  - network connectivity issue"
        err ""
        err "check available releases at:"
        err "  https://github.com/${GITHUB_REPO}/releases"
        exit 1
    fi

    # Extract into install directory
    mkdir -p "$INSTALL_DIR"
    tar -xzf "${tmpdir}/${archive}" --strip-components=1 -C "$INSTALL_DIR" || {
        err "failed to extract archive"
        exit 1
    }

    # Verify binary exists
    if [ ! -f "${INSTALL_DIR}/bin/chic" ]; then
        err "installation failed: chic binary not found at ${INSTALL_DIR}/bin/chic"
        exit 1
    fi
    chmod +x "${INSTALL_DIR}/bin/chic"
    [ -f "${INSTALL_DIR}/bin/chi" ] && chmod +x "${INSTALL_DIR}/bin/chi"

    # macOS: remove quarantine attribute (shouldn't be set via curl, but just in case)
    if [ "$(uname -s)" = "Darwin" ]; then
        xattr -cr "$INSTALL_DIR" 2>/dev/null || true
    fi

    # Verify the binary actually runs
    if ! "${INSTALL_DIR}/bin/chic" --help >/dev/null 2>&1; then
        err "installation completed but chic failed to run"
        if [ "$(uname -s)" = "Darwin" ]; then
            err "try: xattr -cr ${INSTALL_DIR}"
        fi
        exit 1
    fi

    # Configure shell PATH
    configure_shell

    # Done
    info "${GREEN}${BOLD}chi ${VERSION} installed successfully!${RESET}"
    info ""
    info "  chic  ${INSTALL_DIR}/bin/chic"
    info "  chi   ${INSTALL_DIR}/bin/chi"
    info ""

    # Check if already on PATH
    case ":${PATH:-}:" in
        *:"${INSTALL_DIR}/bin":*)
            info "chi is ready to use."
            ;;
        *)
            if [ -n "$MODIFIED_FILES" ]; then
                info "To start using chi, run:"
                info ""
                info "  ${BOLD}source \"${INSTALL_DIR}/env\"${RESET}"
                info ""
                info "This will be automatic in new shells (added to ${MODIFIED_FILES})."
            else
                info "Add chi to your PATH:"
                info ""
                info "  export PATH=\"${INSTALL_DIR}/bin:\$PATH\""
                info ""
            fi
            ;;
    esac
}

main "$@"
