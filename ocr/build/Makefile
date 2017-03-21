#
# Makefile for the OCR Runtime
#
# For OCR licensing terms, see top level LICENSE file.
#
# Author: Ivan Ganev <ivan.b.ganev@intel.com>
#

.PHONY: default
default:
	@echo "Must specify an OCR version/type to build, e.g.:"
	@echo ""
	@echo "   OCR_TYPE=x86 make all"
	@echo ""
	@echo "Valid version are:"
	@echo ""
	@echo "   x86          -- OCR for X86 Linux machines"
	@echo "   x86-mpi      -- OCR for distributed X86 Linux machines using MPI comms"
	@echo "   x86-gasnet   -- OCR for distributed X86 Linux machines using GASNet comms"
	@echo "   x86-phi      -- OCR for X86 Xeon Phi (KNL) Linux machines"
	@echo "   x86-newlib   -- OCR for X86 with newlib to support legacy codes"
	@echo "   tg-x86       -- OCR for Traleika Glacier but emulated on X86 (WIP)"
	@echo "   tg-ce        -- CE parts of OCR for Traleika Glacier"
	@echo "   tg-xe        -- XE parts of OCR for Traleika Glacier"
	@echo "   builder-ce   -- CE struct builder for Traleika Glacier on X86"
	@echo "   builder-xe   -- XE struct builder for Traleika Glacier on X86"
	@echo ""
	@echo "Valid targets are:"
	@echo ""
	@echo "   all          -- Optimized build (alias for static)"
	@echo "   static       -- Optimized static build (produces a .a)"
	@echo "   shared       -- Optimized shared build (produces a .so)"
	@echo "   debug        -- Unoptimized build"
	@echo "   debug-static -- Unoptimized static build"
	@echo "   debug-shared -- Unoptimized shared build"
	@echo "   install      -- Install built distribution"
	@echo "   uninstall    -- Remove installed distribution"
	@echo "   clean        -- Clean-up build objects, etc."
	@echo "   squeaky      -- Clean-up all architectures"
	@echo ""

.PHONY: all
all:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: static
static:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: shared
shared:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: debug
debug:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: debug-static
debug-static:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: debug-shared
debug-shared:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: install
install:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: uninstall
uninstall:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: clean
clean:
	@$(MAKE) -C $(OCR_TYPE) $@

.PHONY: squeaky
squeaky:
	OCR_TYPE=x86 $(MAKE) -C x86 squeaky
	OCR_TYPE=tg-x86 $(MAKE) -C tg-x86 squeaky
	OCR_TYPE=tg-ce $(MAKE) -C tg-ce squeaky
	OCR_TYPE=tg-xe $(MAKE) -C tg-xe squeaky
	OCR_TYPE=builder-ce $(MAKE) -C builder-ce squeaky
	OCR_TYPE=builder-xe $(MAKE) -C builder-xe squeaky
	OCR_TYPE=x86-mpi $(MAKE) -C x86-mpi squeaky
	OCR_TYPE=x86-phi $(MAKE) -C x86-phi squeaky
	OCR_TYPE=x86-newlib $(MAKE) -C x86-newlib squeaky
# Need some more work here so that we can squeaky even when
# some of the gasnet variables are not set.
#	OCR_TYPE=x86-gasnet $(MAKE) -C x86-gasnet squeaky
