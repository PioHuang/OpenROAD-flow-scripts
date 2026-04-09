To execute, 
```
cd ..
```
first build:
```
cmake -S research -B research/build && cmake --build research/build -j
```
then execute:
```
research/build/phys_load research/mempool.json
```