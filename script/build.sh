# run at this repository root
python3 -m venv ~/.venv
source ~/.venv/bin/activate
pip install west
west init -l app
west update
west zephyr-export
west packages pip --install
west build -p always -b native_sim/native/zmk_ipc \
  -s app \
  -d build \
  -- -DZMK_CONFIG=/home/yosuke/work/oneday/config