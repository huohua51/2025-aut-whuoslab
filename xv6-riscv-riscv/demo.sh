#!/bin/bash

echo ""

# 确保没有遗留的QEMU进程
killall qemu-system-riscv64 2>/dev/null || true
sleep 1

# 测试COW版本
echo "测试COW版本..."
sed -i "s/#define USE_COW [01]/#define USE_COW 1/" kernel/vm.c
make CPUS=1 > /dev/null 2>&1

# 运行QEMU并等待done出现或超时
timeout 60 make qemu CPUS=1 > cow_full.txt 2>&1 &
QEMU_PID=$!
sleep 8  # 等待足够时间让测试完成
kill $QEMU_PID 2>/dev/null || true
killall qemu-system-riscv64 2>/dev/null || true
sleep 2

grep -E "(bench_cow|no-touch|touch-|done)" cow_full.txt > cow_result.txt

# 测试传统版本  
echo "测试传统版本..."
sed -i "s/#define USE_COW [01]/#define USE_COW 0/" kernel/vm.c
make CPUS=1 > /dev/null 2>&1

# 运行QEMU并等待done出现或超时
timeout 60 make qemu CPUS=1 > traditional_full.txt 2>&1 &
QEMU_PID=$!
sleep 8  # 等待足够时间让测试完成
kill $QEMU_PID 2>/dev/null || true
killall qemu-system-riscv64 2>/dev/null || true
sleep 2

grep -E "(bench_cow|no-touch|touch-|done)" traditional_full.txt > traditional_result.txt

# 显示对比结果
echo ""
echo "对比结果:"
echo ""

# 提取并显示结果
cow_no=$(grep "no-touch" cow_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)
traditional_no=$(grep "no-touch" traditional_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)
cow_small=$(grep "touch-1pages" cow_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)
traditional_small=$(grep "touch-1pages" traditional_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)
cow_big=$(grep "touch-128pages" cow_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)
traditional_big=$(grep "touch-128pages" traditional_result.txt | grep -o 'total_ticks=[0-9]*' | cut -d'=' -f2)

# 检查是否成功提取到数据
if [ -z "$cow_no" ] || [ -z "$traditional_no" ]; then
    echo "错误：无法提取测试数据！"
    echo ""
    echo "COW结果文件内容："
    cat cow_result.txt
    echo ""
    echo "传统结果文件内容："
    cat traditional_result.txt
    exit 1
fi

echo "no-touch (纯fork):   COW版本=$cow_no ticks, 传统版本=$traditional_no ticks"
echo "touch-1pages (轻写): COW版本=$cow_small ticks, 传统版本=$traditional_small ticks"
echo "touch-128pages (重写): COW版本=$cow_big ticks, 传统版本=$traditional_big ticks"
