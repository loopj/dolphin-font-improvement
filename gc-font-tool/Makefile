# Build Dolphin's gc-font-tool with the proposed rules, and generate both IPL fonts

PKG_CONFIG ?= pkg-config
CXXFLAGS   ?= -O2
CXXFLAGS   += -std=c++11 -Wall $(shell $(PKG_CONFIG) --cflags freetype2)
LDLIBS     += $(shell $(PKG_CONFIG) --libs freetype2)

all: font_western.bin font_japanese.bin

# Build the tool from its single source file
gc-font-tool: gc-font-tool.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

# Generate the Windows-1252 font from the Bold weight, without dithering
font_western.bin: gc-font-tool MPLUSRounded1c-Bold.ttf
	./gc-font-tool b MPLUSRounded1c-Bold.ttf $@

# Generate the Shift JIS font from the Medium weight, without dithering
font_japanese.bin: gc-font-tool MPLUSRounded1c-Medium.ttf
	./gc-font-tool t MPLUSRounded1c-Medium.ttf $@

clean:
	rm -f gc-font-tool font_western.bin font_japanese.bin

.PHONY: all clean
