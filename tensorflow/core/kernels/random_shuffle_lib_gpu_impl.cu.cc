//
// Created by a1 on 7/30/17.
//


#define EIGEN_USE_GPU


#include "tensorflow/core/framework/bfloat16.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/kernels/cuda_device_array_gpu.h"
//#include "tensorflow/core/util/cuda_kernel_helper.h"
#include "tensorflow/core/lib/random/philox_random.h"
#include "tensorflow/core/lib/random/random_distributions.h"
#include "tensorflow/core/util/guarded_philox_random.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/kernels/cuda_device_array.h"
#include "tensorflow/core/kernels/cuda_device_array_gpu.h"

namespace tensorflow {

#if GOOGLE_CUDA

namespace{
class ShuffleHelper{
public:

template<typename T>
__device__ static T max(T x, T y){
  return x > y ? x : y;
}

template<typename T>
__device__ static T min(T x, T y){
  return x < y ? x : y;
}

template<typename T>
__device__ static void init_permutation(T * permutation, int64 size){
  for(int64 i = 0; i < size; i++){
//#pragma unroll
    permutation[i] = static_cast<T>(i);
  }
}

template<typename T>
__device__ static void swap(T * permutation, int64 i, int64 j){
  auto tmp = permutation[i];
  permutation[i] = permutation[j];
  permutation[j] = tmp;
}

template<typename T, typename Uniform>
__device__ static void fisher_yates_random_shuffle(T * permutation, int64 size, Uniform &uniform){
  for(int64 i = 0; i < size - 1; i++){
    int64 j = i + uniform(size - i);
    swap(permutation, i, j);
  }
}

template<typename T, typename Uniform>
__device__ static void merge(T * permutation, int64 start, int64 next, int64 end, Uniform &uniform){
  int i = start;
  int j = next;
  while(true){
    if(uniform(2) == 0){
       if(i == j){
          break;
       }
    }else{
      if(j == end){
        break;
      }
      swap(permutation, i, j++);
    }
    i++;
  }

  while(i != end - 1){
    swap(permutation, i, start + uniform(i - start + 1));
  }
}

template<typename T>
__device__ static void map_block(int block_size, const T * source, int s_index, T * target, int t_index){
  int i = t_index * block_size;
  int j = s_index * block_size;
  int offset = 0;
  while(offset < block_size){
//#pragma unroll
    target[j + offset] = source[i + offset];
    offset++;
  }
}



static int FloorToPowOfTwo(int x){
  CHECK(x >= 1);
  int i = sizeof(int) * 8 - 1;
  while ((x & (0x1 << i)) == 0){
    i--;
  }
  return std::pow(2, i);
}

};


template<typename T>
__global__ static void map_block_kernel(const T * source, int64 size, int64 * mapper, int64 count, T * target){
  int64 thread_count = blockDim.x * gridDim.x;
  int64 thread_scope = count / thread_count;
  if(count % thread_count > 0){
    thread_scope += 1;
  }


  int64 thread_index = blockIdx.x * blockDim.x + threadIdx.x;
  int64 first_block = thread_index * thread_scope;
  if(first_block >= count){
    return;
  }

  int64 last_block = first_block + thread_scope;
  last_block = ShuffleHelper::min(last_block, count);

  int64 block_size = size / count;
  for(int64 i = first_block; i < last_block; i++){
   ShuffleHelper::map_block(block_size, source, i, target, mapper[i]);
  }
}

template<typename T>
__global__ void random_shuffle_kernel(T * permutation, int64 size, bool need_init, CudaDeviceArrayStruct<random::PhiloxRandom> gens){
  if(need_init){
    ShuffleHelper::init_permutation(permutation, size);
  }

  int64 concurrence = blockDim.x * gridDim.x;
  int64 index = blockIdx.x * blockDim.x + threadIdx.x;
  int64 batch_size = size / concurrence;
  int64 batch_remainder = size % concurrence;

  random::PhiloxRandom * local_gens = GetCudaDeviceArrayOnDevice(&gens);
  auto uniform = random::RandomBitsAdapter<random::PhiloxRandom>(&local_gens[index]);
  if(blockIdx.x == gridDim.x - 1 && threadIdx.x == blockDim.x - 1){
    //last thread of last block.
    ShuffleHelper::fisher_yates_random_shuffle(permutation + index * batch_size, batch_size + batch_remainder, uniform);
  }else{
    ShuffleHelper::fisher_yates_random_shuffle(permutation + index * batch_size, batch_size, uniform);
  }
}

template<typename T>
__global__ void merge_shuffle_kernel(T * permutation, int64 size, int64 batch_size, CudaDeviceArrayStruct<random::PhiloxRandom> gens){
  int index = blockIdx.x * blockDim.x + threadIdx.x;

  random::PhiloxRandom * local_gens = GetCudaDeviceArrayOnDevice(&gens);
  auto uniform = random::RandomBitsAdapter<random::PhiloxRandom>(&local_gens[index]);
  if(blockIdx.x == gridDim.x - 1 && threadIdx.x == blockDim.x - 1){
    ShuffleHelper::merge(permutation, 2*index*batch_size, (2*index + 1)*batch_size, size, uniform);
  }else{
    ShuffleHelper::merge(permutation, 2*index*batch_size, (2*index + 1)*batch_size, (2*index + 2)*batch_size, uniform);
  }
}

template<typename T>
void MergeRandomShuffleGPU(OpKernelContext* c,
                           typename TTypes<T, 1>::Vec * permutation,
                           bool need_init,
                           GuardedPhiloxRandom& generator){

  const Eigen::GpuDevice& d = c->eigen_gpu_device();
  const int max_physical_processor_count = d.getNumCudaMultiProcessors();
  const int max_thread_per_physical_processor = d.maxCudaThreadsPerMultiProcessor();
  const int physical_thread_count = max_physical_processor_count * max_thread_per_physical_processor;

  int total = permutation->dimension(0);
  int concurrence = std::min(physical_thread_count, total);
  concurrence = ShuffleHelper::FloorToPowOfTwo(concurrence);

  int threads = ShuffleHelper::FloorToPowOfTwo(max_thread_per_physical_processor);
  int blocks = concurrence / threads;

  int64 size = permutation->dimension(0);
  int64 batch_size = size / concurrence;
  //int64 batch_remainder = size % concurrence;

  CudaDeviceArrayOnHost<random::PhiloxRandom> gens(c, concurrence);
  OP_REQUIRES_OK(c, gens.Init());
  for (int i = 0; i < concurrence; ++i) {
    gens.Set(i, generator.ReserveSamples128(batch_size));
  }
  OP_REQUIRES_OK(c, gens.Finalize());
  random_shuffle_kernel<<<blocks, threads, 0, d.stream()>>>(
                permutation->data(),
                permutation->dimension(0),
                need_init,
                gens.data());

  while(concurrence > 1){
    if(blocks >= 2){
      blocks /= 2;
    }else {
      threads /= 2;
    }
    int merge_concurrence = concurrence / 2;
    CudaDeviceArrayOnHost<random::PhiloxRandom> gens(c, merge_concurrence);
    OP_REQUIRES_OK(c, gens.Init());
    for (int i = 0; i < merge_concurrence; ++i) {
      gens.Set(i, generator.ReserveSamples128(batch_size * 2));
    }
    OP_REQUIRES_OK(c, gens.Finalize());
    merge_shuffle_kernel<<<blocks, threads, 0, d.stream()>>>(permutation,
                                                           size,
                                                           batch_size,
                                                           gens.data());

    concurrence = merge_concurrence;
    batch_size *= 2;
  }
}

template<typename T>
void Assign(OpKernelContext* c,
            const typename TTypes<T, 2>::ConstMatrix& inputs_matrix,
            typename TTypes<int64, 1>::Vec* permutation,
            typename TTypes<T, 2>::Matrix* output){

  auto d = c->eigen_gpu_device();
  const int max_physical_processor_count = d.getNumCudaMultiProcessors();
  const int max_thread_per_physical_processor = d.maxCudaThreadsPerMultiProcessor();
  const int physical_thread_count = max_physical_processor_count * max_thread_per_physical_processor;

  int total = permutation->dimension(0);
  int concurrence = std::min(physical_thread_count, total);
  int threads_per_block = std::min(max_thread_per_physical_processor, total);
  int blocks = std::ceil(concurrence / threads_per_block);
  map_block_kernel<T><<<blocks, threads_per_block, 0, d.stream()>>>(inputs_matrix.data(),
                                                                                  static_cast<int64>(inputs_matrix.size()),
                                                                                  permutation->data(),
                                                                                  static_cast<int64>(permutation->size()),
                                                                                  output->data());
}





}//namespace



template<typename T>
void RandomShuffleVectorGPU(OpKernelContext * c,
                      typename TTypes<T, 1>::Vec * permutation,
                      GuardedPhiloxRandom& generator){
  MergeRandomShuffleGPU<T>(c, permutation, false, generator);
}

#define REGISTER_GPU(T)    											\
  template void RandomShuffleVectorGPU<T>(OpKernelContext *, typename TTypes<T, 1>::Vec*, GuardedPhiloxRandom&);
TF_CALL_GPU_NUMBER_TYPES(REGISTER_GPU);
REGISTER_GPU(bfloat16);
TF_CALL_complex64(REGISTER_GPU);
TF_CALL_complex128(REGISTER_GPU);
#undef REGISTER_GPU


template<typename T>
void RandomShuffleGPU(OpKernelContext* c,
                      const typename TTypes<T, 2>::ConstMatrix& inputs_matrix,
                      typename TTypes<int64, 1>::Vec * permutation,
                      typename TTypes<T, 2>::Matrix * output,
                      GuardedPhiloxRandom& generator){
  MergeRandomShuffleGPU<int64>(c, permutation, true, generator);
  Assign<T>(c, inputs_matrix, permutation, output);
}

#define REGISTER_GPU(T)                                                             \
    template void RandomShuffleGPU<T>(                                          \
                      OpKernelContext* c,                                       \
                      const typename TTypes<T, 2>::ConstMatrix& inputs_matrix,  \
                      typename TTypes<int64, 1>::Vec * permutation,                  \
                      typename TTypes<T, 2>::Matrix * output,                    \
                      GuardedPhiloxRandom& generator);
TF_CALL_GPU_NUMBER_TYPES(REGISTER_GPU);
REGISTER_GPU(bfloat16);
TF_CALL_complex64(REGISTER_GPU);
TF_CALL_complex128(REGISTER_GPU);


#undef REGISTER_GPU


#endif


}
