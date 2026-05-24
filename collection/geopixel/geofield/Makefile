CC       ?= gcc
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -Wpedantic -D__USE_MINGW_ANSI_STDIO

# All headers are local — no extra -I needed
INCDIRS  = -I.

TARGET = geo_field_demo
TEST_TARGET = test_geometry_store_reader

.PHONY: all clean run test

all: $(TARGET)

test: $(TEST_TARGET)

$(TARGET): geo_field_main.c geo_field_core.h $(wildcard *.h)
	$(CC) $(CFLAGS) $(INCDIRS) -o $@ geo_field_main.c

$(TEST_TARGET): test_geometry_store_reader.c geometry_store_reader.h $(wildcard *.h)
	$(CC) $(CFLAGS) $(INCDIRS) -o $@ test_geometry_store_reader.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe *.geofield
