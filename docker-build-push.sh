#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Default settings
DEFAULT_IMAGE="std2main/transmission-seeder"
DEFAULT_TAG="dev"
BUILDER_NAME="transmission-builder"

# Parse arguments
PLATFORMS=""
PUSH=false
CACHE_MODE=local
TAG="${1:-$DEFAULT_TAG}"
shift || true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform|-p)
            PLATFORMS="$2"
            shift 2
            ;;
        --push)
            PUSH=true
            shift
            ;;
        --no-cache)
            CACHE_MODE=none
            shift
            ;;
        --cache-local)
            CACHE_MODE=local
            shift
            ;;
        --cache-registry)
            CACHE_MODE=local
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [TAG] [OPTIONS]"
            echo "Build the Transmission Docker image with buildx cache."
            echo ""
            echo "Arguments:"
            echo "  TAG                  The Docker tag (default: $DEFAULT_TAG)"
            echo ""
            echo "Options:"
            echo "  --platform PLATFORM  Target platform(s), e.g. linux/amd64,linux/arm64"
            echo "  --push               Push image to registry after build"
            echo "  --no-cache           Disable build cache entirely"
            echo "  --cache-local        Use local directory cache (.docker-cache/)"
            echo "  --cache-registry     Use registry cache tags (default)"
            echo ""
            echo "Cache modes:"
            echo "  registry  Push/pull cache from <image>:<tag>-cache-<platform>"
            echo "            Avoids local cache digest inconsistency issues."
            echo "  local     Store cache in .docker-cache/<platform>/"
            echo "            May show warnings on repeated no-change builds."
            echo "  none      Skip all caching"
            echo ""
            echo "Builder instance: $BUILDER_NAME (auto-created if missing)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

# Print banner
echo "=================================================="
echo " Building Transmission Docker Image"
echo " Image: $DEFAULT_IMAGE:$TAG"
echo "=================================================="

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo "Error: docker command not found. Please install Docker." >&2
    exit 1
fi

# Ensure buildx builder instance exists (docker-container driver supports multi-platform + cache export)
if ! docker buildx inspect "$BUILDER_NAME" &>/dev/null; then
    echo "Creating buildx builder instance: $BUILDER_NAME"
    docker buildx create --name "$BUILDER_NAME" --driver docker-container --use
else
    docker buildx use "$BUILDER_NAME"
fi

# Get current git commit hash
COMMIT_HASH="unknown"
if command -v git &> /dev/null && git rev-parse --is-inside-work-tree &> /dev/null; then
    COMMIT_HASH=$(git rev-parse --short HEAD)
fi

echo "Git Commit Hash: $COMMIT_HASH"
echo "Starting docker buildx build..."

# Determine platforms
if [[ -z "$PLATFORMS" ]]; then
    # Default: build for current host architecture only
    PLATFORMS="linux/$(docker info -f '{{.Architecture}}' | sed 's/x86_64/amd64/;s/aarch64/arm64/')"
fi

echo "Platform(s): $PLATFORMS"
echo "Cache mode: $CACHE_MODE"

# Build cache arguments
CACHE_ARGS=()
if [[ "$CACHE_MODE" == "local" ]]; then
    IFS=',' read -ra PLAT_LIST <<< "$PLATFORMS"
    for plat in "${PLAT_LIST[@]}"; do
        CACHE_KEY="$(echo "$plat" | tr '/' '_')"
        CACHE_PATH=".docker-cache/${CACHE_KEY}"
        mkdir -p "$CACHE_PATH"
        if [[ -f "$CACHE_PATH/index.json" ]]; then
            CACHE_ARGS+=(--cache-from=type=local,src="$CACHE_PATH")
        fi
        CACHE_ARGS+=(--cache-to=type=local,dest="$CACHE_PATH",mode=max)
    done
elif [[ "$CACHE_MODE" == "registry" ]]; then
    # Use per-platform cache tags in the same registry.
    # type=registry is additive — no digest mismatch warnings.
    IFS=',' read -ra PLAT_LIST <<< "$PLATFORMS"
    for plat in "${PLAT_LIST[@]}"; do
        CACHE_KEY="$(echo "$plat" | tr '/' '_')"
        CACHE_TAG="${DEFAULT_IMAGE}:${TAG}-cache-${CACHE_KEY}"
        CACHE_ARGS+=(--cache-from=type=registry,ref="$CACHE_TAG")
        CACHE_ARGS+=(--cache-to=type=registry,ref="$CACHE_TAG",mode=max)
    done
fi

# Load vs push: single-platform can load locally, multi-platform must push
LOAD_OR_PUSH=()
if [[ "$PUSH" == true ]]; then
    LOAD_OR_PUSH=(--push)
elif [[ "$PLATFORMS" != *","* ]]; then
    LOAD_OR_PUSH=(--load)
else
    echo "Warning: multi-platform build without --push; images will not be loaded locally."
    echo "Use --push to push to registry, or build a single platform."
    LOAD_OR_PUSH=(--push)
fi

# Run docker buildx build
docker buildx build \
    --builder "$BUILDER_NAME" \
    --platform "$PLATFORMS" \
    --build-arg COMMIT_HASH="$COMMIT_HASH" \
    --build-arg ENABLE_TESTS=OFF \
    --provenance=false \
    "${CACHE_ARGS[@]}" \
    "${LOAD_OR_PUSH[@]}" \
    -t "$DEFAULT_IMAGE:$TAG" \
    -f Dockerfile .

if [[ "$PUSH" == true ]]; then
    echo "Build & Push successful!"
    IFS=',' read -ra PLAT_LIST <<< "$PLATFORMS"
    for plat in "${PLAT_LIST[@]}"; do
        echo "  Pushed: $DEFAULT_IMAGE:$TAG ($plat)"
    done
else
    echo "Build successful! Image: $DEFAULT_IMAGE:$TAG"
fi
if [[ "$CACHE_MODE" == "registry" ]]; then
    IFS=',' read -ra PLAT_LIST <<< "$PLATFORMS"
    for plat in "${PLAT_LIST[@]}"; do
        CACHE_KEY="$(echo "$plat" | tr '/' '_')"
        echo " Cache tag: ${DEFAULT_IMAGE}:${TAG}-cache-${CACHE_KEY}"
    done
fi
echo "=================================================="
