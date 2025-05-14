all: clean install build

clean:
	rm -rf build/ dist/ analyze/
	rm -rf gnubg/gnubg.egg-info

install:
	pip install --upgrade pip setuptools wheel cibuildwheel twine

build:
	python3 setup.py sdist bdist_wheel

test:
	pip install dist/*.whl
	cd tests && python3 test.py

twine:
	twine upload dist/*

# Check-in code after formatting
checkin: ## Perform a check-in after formatting the code
    ifndef COMMIT_MESSAGE
		$(eval COMMIT_MESSAGE := $(shell bash -c 'read -e -p "Commit message: " var; echo $$var'))
    endif
	@git add --all; \
	  git commit -m "$(COMMIT_MESSAGE)"; \
	  git push
