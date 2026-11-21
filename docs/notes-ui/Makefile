# The reference renderer for docs/notes-ui-spec.md.
#
# It compiles against the SDK's own hg::Canvas and hg::font, so the wireframes in
# the spec are renders of the real layout rather than drawings of it, and every
# string is width-checked against the slot that holds it.
CORE    := ../../firmware/core
CXX     ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

SOURCES := $(CORE)/src/canvas.cpp $(CORE)/src/font5x7.cpp \
           $(CORE)/src/font_cjk.cpp $(CORE)/src/font_cjk_data.cpp

render: render.cpp $(SOURCES)
	$(CXX) $(CXXFLAGS) -I$(CORE)/include -I$(CORE)/src render.cpp $(SOURCES) -o $@

.PHONY: clean
clean:
	$(RM) render
	$(RM) -r out
