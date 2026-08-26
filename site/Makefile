# Keel landing page
#
# Builds the deployable site into build/. Mirrors the Hull site pattern
# so deployment can stay consistent across artalis-io repos.

.PHONY: all build check clean serve

BUILD_DIR := build
STATIC_FILES := index.html keel.css mark.svg og-card.svg

all: build

# Offline, dependency-free validation (core Perl only). Self-cleaning: the
# reproducibility check builds into temporary directories and never reads,
# writes, or removes an existing build/.
check:
	@perl check_site.pl

build:
	@mkdir -p $(BUILD_DIR)
	@cp $(STATIC_FILES) $(BUILD_DIR)/
	@echo "Site built in $(BUILD_DIR)/"
	@echo "  Files: $$(find $(BUILD_DIR) -type f | wc -l | tr -d ' ')"
	@find $(BUILD_DIR) -type f -exec ls -l {} \;

serve: build
	@echo "Serving $(BUILD_DIR)/ at http://localhost:8000"
	@cd $(BUILD_DIR) && python3 -m http.server 8000

clean:
	rm -rf $(BUILD_DIR)
