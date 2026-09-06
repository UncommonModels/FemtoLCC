# FemtoLCC — fabrication outputs and documentation site.
#
# The KiCad CLI ships inside the Flatpak runtime on this machine, and the
# Flatpak sandbox cannot see /tmp or paths outside $HOME. Every kicad-cli
# output path below is therefore repo-relative; do not point them at /tmp.

PROJECT   := FemtoLCC
PCB       := $(PROJECT).kicad_pcb
SCH       := $(PROJECT).kicad_sch
SCHEMATICS := $(wildcard *.kicad_sch)

PROD      := production
GERBERDIR := $(PROD)/gerbers
SITE      := site
SOFTWARE  := software
PUBLIC    := $(SITE)/public

# Prefer a native kicad-cli, fall back to the Flatpak.
KICAD_CLI ?= $(shell command -v kicad-cli >/dev/null 2>&1 \
	&& echo kicad-cli \
	|| echo 'flatpak run --command=kicad-cli org.kicad.KiCad')
HUGO      ?= hugo
SERVE_PORT ?= 1313
PYTHON    ?= python3

# Gerber layers uploaded to the fab, in stack order.
LAYERS := F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,F.Paste,B.Paste,Edge.Cuts

# Sentinel outputs standing in for the many files each export writes.
GERBER_JOB := $(GERBERDIR)/$(PROJECT)-job.gbrjob
DRILL_PTH  := $(GERBERDIR)/$(PROJECT)-PTH.drl
ZIP        := $(PROD)/$(PROJECT)-gerbers.zip
BOM        := $(PROD)/$(PROJECT)-BOM.csv
BOM_FULL   := $(PROD)/$(PROJECT)-parts-reference.csv
UPLOAD     := $(PROD)/upload
PARTMAP    := tools/lcsc-parts.csv
DNPLIST    := tools/do-not-populate.txt
CPL        := $(PROD)/$(PROJECT)-CPL.csv
RENDER     := $(PROD)/render-top.png
RENDER_BOT := $(PROD)/render-bottom.png
SCH_PDF    := $(PROD)/$(PROJECT)-schematic.pdf

.DEFAULT_GOAL := help

## help: list the available targets
help:
	@echo 'FemtoLCC make targets:'
	@sed -n 's/^## \([a-z-]*\): /  \1|/p' $(MAKEFILE_LIST) | column -t -s'|'

## all: run the checks, build every fab output and the docs site
all: check production docs

# --------------------------------------------------------------------------
# Fabrication outputs
# --------------------------------------------------------------------------

## production: every file needed to order the board (gerbers, drill, BOM, CPL, render)
production: $(ZIP) $(BOM) $(BOM_FULL) $(CPL) $(RENDER) $(RENDER_BOT) upload

## gerbers: plot RS-274X gerbers for the fab layers
gerbers: $(GERBER_JOB)
$(GERBER_JOB): $(PCB)
	@mkdir -p $(GERBERDIR)
	$(KICAD_CLI) pcb export gerbers -o $(GERBERDIR)/ -l $(LAYERS) \
		--no-x2 --no-netlist --subtract-soldermask --check-zones --precision 6 $(PCB)

## drill: generate Excellon drill files and drill maps
drill: $(DRILL_PTH)
$(DRILL_PTH): $(PCB)
	@mkdir -p $(GERBERDIR)
	$(KICAD_CLI) pcb export drill -o $(GERBERDIR)/ --format excellon \
		--drill-origin absolute --excellon-units mm --excellon-separate-th \
		--generate-map --map-format gerberx2 $(PCB)

## zip: bundle the gerbers and drill files for upload
zip: $(ZIP)
$(ZIP): $(GERBER_JOB) $(DRILL_PTH)
	rm -f $@
	cd $(GERBERDIR) && zip -q -X ../$(notdir $@) \
		$(PROJECT)-F_Cu.gtl $(PROJECT)-In1_Cu.g1 $(PROJECT)-In2_Cu.g2 $(PROJECT)-B_Cu.gbl \
		$(PROJECT)-F_Mask.gts $(PROJECT)-B_Mask.gbs \
		$(PROJECT)-F_Silkscreen.gto $(PROJECT)-B_Silkscreen.gbo \
		$(PROJECT)-F_Paste.gtp $(PROJECT)-B_Paste.gbp \
		$(PROJECT)-Edge_Cuts.gm1 $(PROJECT)-NPTH.drl $(PROJECT)-PTH.drl
	@echo 'Wrote $@'

# The flat, one-row-per-part export both BOM targets are built from.
$(PROD)/.bom-flat.csv: $(SCHEMATICS)
	@mkdir -p $(PROD)
	$(KICAD_CLI) sch export bom -o $@ \
		--fields 'Reference,Value,Footprint,LCSC,Manufacturer Part Number,MANUFACTURER' \
		--labels 'Reference,Value,Footprint,LCSC,MPN,Manufacturer' \
		--group-by '' --exclude-dnp $(SCH)

## bom: export the JLCPCB assembly BOM, covering exactly the placed parts
# JLC rejects an upload whose BOM and CPL disagree on designators, so this is
# filtered against the CPL and depends on it.
bom: $(BOM)
$(BOM): $(PROD)/.bom-flat.csv $(CPL) $(PARTMAP) $(DNPLIST) tools/bom2jlc.py tools/partmap.py
	$(PYTHON) tools/bom2jlc.py $< $@ --parts $(PARTMAP) --placements $(CPL) --dnp $(DNPLIST)
	@echo 'Wrote $@'

## bom-full: export the complete parts list including hand-fitted parts (NOT for upload)
bom-full: $(BOM_FULL)
$(BOM_FULL): $(PROD)/.bom-flat.csv $(PARTMAP) tools/bom2jlc.py tools/partmap.py
	$(PYTHON) tools/bom2jlc.py $< $@ --parts $(PARTMAP)
	@echo 'Wrote $@'

## cpl: export the JLCPCB pick-and-place file (top side, assembled parts only)
cpl: $(CPL)
$(CPL): $(PCB) $(PARTMAP) $(DNPLIST) tools/pos2jlc.py tools/partmap.py
	@mkdir -p $(PROD)
	$(KICAD_CLI) pcb export pos -o $(PROD)/.pos-raw.csv \
		--format csv --units mm --side front --exclude-dnp $(PCB)
	$(PYTHON) tools/pos2jlc.py $(PROD)/.pos-raw.csv $@ --parts $(PARTMAP) --dnp $(DNPLIST)
	@rm -f $(PROD)/.pos-raw.csv
	@echo 'Wrote $@'

## render: ray-traced 3D render of the assembled board (top)
render: $(RENDER)
$(RENDER): $(PCB)
	@mkdir -p $(PROD)
	$(KICAD_CLI) pcb render -o $@ --side top --quality high \
		--width 1600 --height 1200 --background transparent $(PCB)

## render-bottom: ray-traced 3D render of the board underside
render-bottom: $(RENDER_BOT)
$(RENDER_BOT): $(PCB)
	@mkdir -p $(PROD)
	$(KICAD_CLI) pcb render -o $@ --side bottom --quality high \
		--width 1600 --height 1200 --background transparent $(PCB)

## schematic: export the full schematic as a PDF
schematic: $(SCH_PDF)
$(SCH_PDF): $(SCHEMATICS)
	@mkdir -p $(PROD)
	$(KICAD_CLI) sch export pdf -o $@ $(SCH)

# --------------------------------------------------------------------------
# Design checks
# --------------------------------------------------------------------------

## check: run both ERC and DRC, failing on any violation
check: erc drc

## stock-check: query JLC live for the BOM's parts (BOARDS=n, default 10)
BOARDS ?= 10
stock-check: $(BOM)
	$(PYTHON) tools/jlcstock.py --boards $(BOARDS) $(BOM)

## erc: electrical rules check on the schematic
erc:
	@mkdir -p $(PROD)
	$(KICAD_CLI) sch erc --exit-code-violations --severity-error \
		-o $(PROD)/$(PROJECT)-erc.rpt $(SCH)

## drc: design rules check on the board, including unconnected nets
# --refill-zones matters: without it, zones saved before a via was added still
# cover that via and DRC reports phantom 0.0000 mm clearance errors. The gerber
# export refills for the same reason (--check-zones). Neither writes the board.
drc:
	@mkdir -p $(PROD)
	$(KICAD_CLI) pcb drc --exit-code-violations --severity-error --refill-zones \
		-o $(PROD)/$(PROJECT)-drc.rpt $(PCB)

# --------------------------------------------------------------------------
# Firmware
#
# Delegated to software/Makefile, which owns the arduino-cli invocation. Pass
# PORT= and FQBN= straight through.
# --------------------------------------------------------------------------

## firmware: compile the ESP32-C6 firmware
firmware:
	$(MAKE) -C $(SOFTWARE) build

## flash: build and upload the firmware (PORT=/dev/ttyUSB0)
flash:
	$(MAKE) -C $(SOFTWARE) flash

## monitor: open the firmware serial console
monitor:
	$(MAKE) -C $(SOFTWARE) monitor

## firmware-deps: install the Arduino core the firmware needs
firmware-deps:
	$(MAKE) -C $(SOFTWARE) deps

# --------------------------------------------------------------------------
# Documentation site
# --------------------------------------------------------------------------

## docs: build the Hugo documentation site into site/public
# --cleanDestinationDir removes output for pages that no longer exist; without
# it a deleted page lingers in site/public and keeps being served.
docs: site-assets
	$(HUGO) --source $(SITE) --minify --cleanDestinationDir

## serve: run the Hugo dev server with live reload (override with SERVE_PORT=)
serve: site-assets
	$(HUGO) server --source $(SITE) --buildDrafts --disableFastRender --port $(SERVE_PORT)

## site-assets: render the board preview and stage it into the site
# $(RENDER) is a real prerequisite, so `make docs` and `make serve` re-render
# whenever the board is newer than the image, and skip the ~18 s render when it
# is not. Only the top view is used by the site; `make render-bottom` still
# produces production/render-bottom.png for the fab package. The result is committed under site/static so the Pages workflow can
# build the site without running KiCad - CI calls hugo directly, not this target.
site-assets: $(RENDER)
	@mkdir -p $(SITE)/static/img
	@cp $(RENDER) $(SITE)/static/img/render-top.png
	@echo 'Staged board preview from $(RENDER)'

# --------------------------------------------------------------------------
# Housekeeping
# --------------------------------------------------------------------------

## upload: stage exactly the three files JLCPCB wants, and nothing else
# Uploading the full parts list instead of the assembly BOM makes JLC report
# "designators don't exist in the CPL file", so keep the upload set unambiguous.
upload: $(ZIP) $(BOM) $(CPL)
	@rm -rf $(UPLOAD) && mkdir -p $(UPLOAD)
	@cp $(ZIP) $(BOM) $(CPL) $(UPLOAD)/
	@echo 'Upload these three files to JLCPCB:'
	@ls -1 $(UPLOAD) | sed 's/^/    /'

## clean: remove generated fab outputs and the built site
clean:
	rm -rf $(GERBERDIR) $(UPLOAD) $(ZIP) $(BOM) $(BOM_FULL) $(CPL) $(RENDER) $(RENDER_BOT) $(SCH_PDF)
	rm -rf $(PUBLIC) $(SITE)/resources $(SITE)/.hugo_build.lock
	$(MAKE) -C $(SOFTWARE) clean
	rm -f $(PROD)/.bom-flat.csv $(PROD)/.pos-raw.csv
	rm -f $(PROD)/$(PROJECT)-erc.rpt $(PROD)/$(PROJECT)-drc.rpt

## distclean: clean, and drop the copies staged under site/static
distclean: clean
	rm -f $(SITE)/static/img/render-*.png

.PHONY: all help production gerbers drill zip bom bom-full cpl render render-bottom \
	schematic check erc drc stock-check upload firmware flash monitor firmware-deps \
	docs serve site-assets clean distclean
