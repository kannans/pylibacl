SPHINXOPTS    = -W
SPHINXBUILD   = sphinx-build
DOCDIR        = doc
DOCHTML       = $(DOCDIR)/html
DOCTREES      = $(DOCDIR)/doctrees
ALLSPHINXOPTS = -d $(DOCTREES) $(SPHINXOPTS) $(DOCDIR)

MODNAME = posix1e.so
RSTFILES = doc/index.rst doc/module.rst NEWS README doc/conf.py

all: doc test

$(MODNAME): acl.c
	./setup.py build_ext --inplace

$(DOCHTML)/index.html: $(MODNAME) $(RSTFILES)
	$(SPHINXBUILD) -b html $(ALLSPHINXOPTS) $(DOCHTML)
	touch $@

doc: $(DOCHTML)/index.html

test:
	for ver in 2.4 2.5 2.6 2.7 3.0 3.1 3.2 3.3 3.4; do \
	  if type python$$ver >/dev/null; then \
	    echo Testing with python$$ver; \
	    python$$ver ./setup.py test -q; \
          fi; \
	done
	@if type pypy >/dev/null; then \
	  echo Testing with pypy; \
	pypy ./setup.py test -q; \
	fi


clean:
	rm -rf $(DOCHTML) $(DOCTREES)
	rm -f $(MODNAME)
	rm -rf build

.PHONY: doc test clean
