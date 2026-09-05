#!/bin/sh
# Package and ad-hoc sign a relocatable macOS OFX bundle.
set -eu

BUNDLE_DIR="$1"
PLUGIN_NAME="$2"

if [ -z "$BUNDLE_DIR" ] || [ -z "$PLUGIN_NAME" ]; then
    echo "Usage: $0 <bundle_dir> <plugin_name>"
    exit 1
fi

BUNDLE_MACOS="$BUNDLE_DIR/Contents/MacOS"
BUNDLE_LIBRARIES="$BUNDLE_DIR/Contents/Libraries"
OFX_BINARY="$BUNDLE_MACOS/$PLUGIN_NAME.ofx"

mkdir -p "$BUNDLE_LIBRARIES"

if [ "$(uname -s)" = "Darwin" ]; then
    # Source/build files can inherit FinderInfo/resource-fork xattrs. They are
    # not part of the OFX bundle and make codesign reject the Mach-O.
    xattr -cr "$BUNDLE_DIR" 2>/dev/null || true
fi

# Bundle non-system dylibs recursively. The red test has none, but this is the
# packaging contract we will retain when FilmViz dependencies return.
changed=1
while [ "$changed" -eq 1 ]; do
    changed=0
    for target in "$OFX_BINARY" "$BUNDLE_LIBRARIES"/*.dylib; do
        [ -f "$target" ] || continue
        dependencies=$(otool -L "$target" | awk 'NR > 1 { print $1 }')
        for dependency in $dependencies; do
            case "$dependency" in
                /usr/lib/*|/System/*)
                    ;;
                @loader_path/*|@executable_path/*)
                    ;;
                @rpath/*)
                    filename=$(basename "$dependency")
                    bundled="$BUNDLE_LIBRARIES/$filename"

                    if [ -f "$bundled" ]; then
                        if [ "$target" = "$OFX_BINARY" ]; then
                            replacement="@loader_path/../Libraries/$filename"
                        else
                            replacement="@loader_path/$filename"
                        fi
                        install_name_tool -change "$dependency"                             "$replacement" "$target"
                    else
                        found=""
                        rpaths=$(otool -l "$target" | awk '
                            $1 == "cmd" && $2 == "LC_RPATH" { in_rpath=1; next }
                            in_rpath && $1 == "path" { print $2; in_rpath=0 }
                        ')
                        for rpath in $rpaths; do
                            case "$rpath" in
                                @loader_path*)
                                    base=$(dirname "$target")
                                    suffix=${rpath#@loader_path}
                                    candidate="$base$suffix/$filename"
                                    ;;
                                @executable_path*)
                                    base="$BUNDLE_MACOS"
                                    suffix=${rpath#@executable_path}
                                    candidate="$base$suffix/$filename"
                                    ;;
                                *)
                                    candidate="$rpath/$filename"
                                    ;;
                            esac

                            if [ -f "$candidate" ]; then
                                found="$candidate"
                                break
                            fi
                        done

                        if [ -n "$found" ]; then
                            cp -L "$found" "$bundled"
                            chmod +w "$bundled"
                            codesign --remove-signature "$bundled" 2>/dev/null || true
                            if [ "$target" = "$OFX_BINARY" ]; then
                                replacement="@loader_path/../Libraries/$filename"
                            else
                                replacement="@loader_path/$filename"
                            fi
                            install_name_tool -change "$dependency"                                 "$replacement" "$target"
                            changed=1
                        else
                            echo "error: unresolved @rpath dependency:"
                            echo "  target: $target"
                            echo "  dependency: $dependency"
                            exit 1
                        fi
                    fi
                    ;;
                *)
                    if [ -f "$dependency" ]; then
                        filename=$(basename "$dependency")
                        bundled="$BUNDLE_LIBRARIES/$filename"
                        if [ ! -f "$bundled" ]; then
                            cp -L "$dependency" "$bundled"
                            chmod +w "$bundled"
                            codesign --remove-signature "$bundled" 2>/dev/null || true
                            changed=1
                        fi
                        install_name_tool -change "$dependency" \
                            "@loader_path/../Libraries/$filename" "$target"
                    fi
                    ;;
            esac
        done
    done
done

for target in "$BUNDLE_LIBRARIES"/*.dylib; do
    [ -f "$target" ] || continue
    install_name_tool -id \
        "@loader_path/../Libraries/$(basename "$target")" "$target"
done


# All dependency rewriting must happen before final signing. Remove inherited
# signatures first so install_name_tool does not repeatedly invalidate them.
for target in "$OFX_BINARY" "$BUNDLE_LIBRARIES"/*.dylib; do
    [ -f "$target" ] || continue
    codesign --remove-signature "$target" 2>/dev/null || true
done

# Normalize references between bundled dylibs. The OFX binary lives in
# Contents/MacOS, while all non-system dylibs live together in
# Contents/Libraries.
for target in "$OFX_BINARY" "$BUNDLE_LIBRARIES"/*.dylib; do
    [ -f "$target" ] || continue

    dependencies=$(otool -L "$target" | awk 'NR > 1 { print $1 }')
    for dependency in $dependencies; do
        filename=$(basename "$dependency")
        [ -f "$BUNDLE_LIBRARIES/$filename" ] || continue

        if [ "$target" = "$OFX_BINARY" ]; then
            replacement="@loader_path/../Libraries/$filename"
        else
            replacement="@loader_path/$filename"
        fi

        case "$dependency" in
            /usr/lib/*|/System/*)
                ;;
            *)
                install_name_tool -change "$dependency" "$replacement" "$target" 2>/dev/null || true
                ;;
        esac
    done
done

if [ "$(uname -s)" = "Darwin" ]; then
    xattr -cr "$BUNDLE_DIR" 2>/dev/null || true
fi

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
for target in "$BUNDLE_LIBRARIES"/*.dylib "$OFX_BINARY"; do
    [ -f "$target" ] || continue
    codesign --force --sign "$CODESIGN_IDENTITY" --timestamp=none "$target"
done
codesign --force --sign "$CODESIGN_IDENTITY" --timestamp=none "$BUNDLE_DIR"
codesign --verify --deep --strict "$BUNDLE_DIR"

echo "[package_bundle.sh] Packaged, signed, and verified $BUNDLE_DIR"
