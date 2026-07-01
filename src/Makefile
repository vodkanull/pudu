CC = cc
CFLAGS = -DWLR_USE_UNSTABLE -I. -Iprotocols/generated $(shell pkg-config --cflags wlroots-0.19 wayland-server libinput cairo)
LIBS = $(shell pkg-config --libs wlroots-0.19 wayland-server xkbcommon libinput cairo) -lpixman-1

BUILD = build
SRC_DIR = .
PROTO_DIR = protocols
GEN_DIR = protocols/generated

# Archivos de protocolo generados
PROTOCOLS = $(GEN_DIR)/xdg-shell-protocol.h \
            $(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.h \
            $(GEN_DIR)/pointer-constraints-unstable-v1-protocol.h \
            $(GEN_DIR)/ext-session-lock-v1-protocol.h \
            $(GEN_DIR)/ext-workspace-v1-protocol.h

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/keyboard.c \
       $(SRC_DIR)/cursor.c \
       $(SRC_DIR)/toplevel.c \
       $(SRC_DIR)/layout.c \
       $(SRC_DIR)/workspace.c \
       $(SRC_DIR)/output.c \
       $(SRC_DIR)/layer_shell.c \
       $(SRC_DIR)/session_lock.c \
       $(SRC_DIR)/pudu_config.c \
       $(SRC_DIR)/bindings.c \
       $(SRC_DIR)/popup.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

PROTO_OBJ = $(BUILD)/ext-workspace-v1-protocol.o

$(BUILD)/pudu: $(OBJS) $(PROTOCOLS) $(PROTO_OBJ)
	@mkdir -p $(BUILD)
	$(CC) -o $@ $(OBJS) $(PROTO_OBJ) $(CFLAGS) $(LIBS)

$(BUILD)/default_config.inc: $(SRC_DIR)/config
	@mkdir -p $(BUILD)
	printf 'static const char default_config[] =\n' > $@
	sed 's/$$/\\n/' $< | sed 's/.*/"&"/' >> $@
	printf ';\n' >> $@

$(BUILD)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/pudu.h $(BUILD)/default_config.inc $(PROTOCOLS)
	@mkdir -p $(BUILD)
	$(CC) -c -o $@ $< $(CFLAGS)

# Reglas para generar los headers de los protocolos
$(GEN_DIR)/xdg-shell-protocol.h: $(PROTO_DIR)/xdg-shell.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner server-header $< $@

$(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.h: $(PROTO_DIR)/wlr-layer-shell-unstable-v1.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner server-header $< $@

$(GEN_DIR)/pointer-constraints-unstable-v1-protocol.h: $(PROTO_DIR)/pointer-constraints-unstable-v1.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner server-header $< $@

$(GEN_DIR)/ext-session-lock-v1-protocol.h: $(PROTO_DIR)/ext-session-lock-v1.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner server-header $< $@

$(GEN_DIR)/ext-workspace-v1-protocol.h: $(PROTO_DIR)/ext-workspace-v1.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner server-header $< $@

$(GEN_DIR)/ext-workspace-v1-protocol.c: $(PROTO_DIR)/ext-workspace-v1.xml
	@mkdir -p $(GEN_DIR)
	wayland-scanner private-code $< $@

$(PROTO_OBJ): $(GEN_DIR)/ext-workspace-v1-protocol.c
	@mkdir -p $(BUILD)
	$(CC) -c -o $@ $< $(CFLAGS)

install:
	install -Dm755 $(BUILD)/pudu /usr/local/bin/pudu
	install -Dm644 $(SRC_DIR)/pudu.desktop /usr/share/wayland-sessions/pudu.desktop

clean:
	rm -rf $(BUILD) $(GEN_DIR)

.PHONY: clean install
