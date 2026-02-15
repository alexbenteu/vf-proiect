CXX ?= g++
NVCC ?= nvcc

BUILD_DIR := build
INSTINCT_GPU_TARGET := $(BUILD_DIR)/instinct_gpu_service

.PHONY: all clean instinct-gpu-service

all: instinct-gpu-service

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(INSTINCT_GPU_TARGET): src/instinct_gpu_service.cu src/cnf.cpp | $(BUILD_DIR)
	$(NVCC) -O2 -std=c++17 -Iinclude -gencode arch=compute_80,code=sm_80 src/instinct_gpu_service.cu src/cnf.cpp -o $@

instinct-gpu-service: $(INSTINCT_GPU_TARGET)

clean:
	rm -rf $(BUILD_DIR)
