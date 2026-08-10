source /usr/local/Ascend/ascend-toolkit/set_env.sh
export PLATFORM=ascend
export BUILD_UCM_ASU=1

pip install -v -e . --no-build-isolation
