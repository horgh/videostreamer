CC=gcc

# Reviewed warnings for gcc 6.2.1
CFLAGS = \
	-std=c11 -g -ggdb -pedantic -pedantic-errors \
	-Werror -Wall -Wextra \
	-Wformat=2 \
	-Wformat-signedness \
	-Wnull-dereference \
	-Winit-self \
	-Wmissing-include-dirs \
	-Wshift-overflow=2 \
	-Wswitch-default \
	-Wswitch-enum \
	-Wunused-const-variable=2 \
	-Wuninitialized \
	-Wunknown-pragmas \
	-Wstrict-overflow=5 \
	-Wsuggest-attribute=pure \
	-Wsuggest-attribute=const \
	-Wsuggest-attribute=noreturn \
	-Wsuggest-attribute=format \
	-Warray-bounds=2 \
	-Wduplicated-cond \
	-Wfloat-equal \
	-Wundef \
	-Wshadow \
	-Wbad-function-cast \
	-Wcast-qual \
	-Wcast-align \
	-Wwrite-strings \
	-Wconversion \
	-Wjump-misses-init \
	-Wlogical-op \
	-Waggregate-return \
	-Wcast-align \
	-Wstrict-prototypes \
	-Wold-style-definition \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wpacked \
	-Wredundant-decls \
	-Wnested-externs \
	-Winline \
	-Winvalid-pch \
	-Wstack-protector

TARGETS=remux_example

all: $(TARGETS)

remux_example: remux_example.c \
	../../videostreamer.c ../../videostreamer.h
	$(CC) $(CFLAGS) -I../../ -o $@ $< ../../videostreamer.c -lavformat \
		-lavdevice -lavcodec -lavutil

clean:
	rm -f $(TARGETS)
