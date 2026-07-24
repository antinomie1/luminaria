tinyluminaria = executable('tinyluminaria', 'tinyluminaria.cpp', dependencies : luminaria_dep)

# Smoke test: the whole stack wires up, runs the loop, and exits cleanly.
test('tinyluminaria-smoke', tinyluminaria, env : ['LUMINARIA_EXIT_MS=200', 'LUMINARIA_BACKEND=headless'], timeout : 10)

executable('luminaria-drm-demo', 'drm_demo.cpp', dependencies : luminaria_dep)
executable('luminaria-tty', 'tty_compositor.cpp', dependencies : luminaria_dep)
