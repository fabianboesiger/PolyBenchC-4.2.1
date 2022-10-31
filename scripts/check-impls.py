import os


def main():
    check_benches("./linear-algebra/blas/gemm", "./linear-algebra/solvers/ludcmp")


def check_benches(*dirs):
    for dir in dirs:
        check_bench(dir)


def check_bench(dir):
    dir = os.path.normpath(dir)
    bench_name = os.path.basename(dir);

    print("Checking outputs of bench '{}'".format(bench_name))

    # Assume base benchmark has same name as containing directory
    base_name = bench_name + ".c"
    base_impl = dir + "/" + base_name
    other_impls = map(lambda name: dir + "/" + name, filter(lambda name: name.endswith(".c") and name != base_name, os.listdir(dir)))

    print("Checking base implementation: {}".format(base_impl), end =" ... ", flush=True)
    base_result = run_impl(base_impl)
    print("Done!")

    for other_impl in other_impls:
        print("Checking other implementation: {}".format(other_impl), end =" ... ", flush=True)
        other_result = run_impl(other_impl)

        if same_arrays(base_result, other_result):
            printGreen("Success!")
        else:
            printRed("Failure!")



def run_impl(impl):
    header = impl.replace(".c", "")

    # Compile implementation
    os.system("gcc -O0 -mfma -fopenmp -I utilities -I {} utilities/polybench.c {} -DPOLYBENCH_DUMP_ARRAYS -o executable".format(header, impl))
    # Run and get output
    output = os.popen("./executable 2>&1").read()
    digits = [float(x) for x in output.split() if isfloat(x)]
    return digits


def isfloat(num):
    try:
        float(num)
        return True
    except ValueError:
        return False


def same_arrays(arr1, arr2):
    if len(arr1) != len(arr2):
        return False

    for x1, x2 in zip(arr1, arr2):
        if abs(x1 - x2) > 0.000000001:
            return False

    return True

def printRed(text): print("\033[91m{}\033[00m" .format(text))
def printGreen(text): print("\033[92m{}\033[00m" .format(text))
 

if __name__ == "__main__":
    main()