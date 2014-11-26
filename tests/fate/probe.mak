FATE_PROBE_FORMAT += fate-probe-format-roundup997
fate-probe-format-roundup997:  REF = mpeg

FATE_PROBE_FORMAT += fate-probe-format-roundup1383
fate-probe-format-roundup1383: REF = mp3

FATE_PROBE_FORMAT += fate-probe-format-roundup1414
fate-probe-format-roundup1414: REF = mpeg

FATE_PROBE_FORMAT += fate-probe-format-roundup2015
fate-probe-format-roundup2015: REF = dv

# extra target neede because otherwise avprobe returns the absolute file path
tests/data/metadata.mov: tests/data
	cp $(TARGET_SAMPLES)/mov/metadata.mov tests/data/

FATE_PROBE_FORMAT-$(CONFIG_MOV_DEMUXER) += fate-probe-format-mov
fate-probe-format-mov: tests/data/metadata.mov
fate-probe-format-mov: CMD = run avprobe$(EXESUF) -show_format -v 0 tests/data/metadata.mov

FATE_SAMPLES-$(CONFIG_AVPROBE) += $(FATE_PROBE_FORMAT) $(FATE_PROBE_FORMAT-yes)
fate-probe-format: $(FATE_PROBE_FORMAT) $(FATE_PROBE_FORMAT-yes)

$(FATE_PROBE_FORMAT): avprobe$(EXESUF)
$(FATE_PROBE_FORMAT): CMP = oneline
fate-probe-format-%: CMD = probefmt $(TARGET_SAMPLES)/probe-format/$(@:fate-probe-format-%=%)
