SPARK_MASTER=$1
SPARK_HOME=$2
EXECUTOR_MEMORY=$3
export PYTHONPATH='.':$PYTHONPATH
echo $@
echo "Starting Spark Job as follows ....."
echo $SPARK_HOME/bin/spark-submit --master $SPARK_MASTER --executor-memory $EXECUTOR_MEMORY clustering.py ${@:4}

$SPARK_HOME/bin/spark-submit --master $SPARK_MASTER --executor-memory $EXECUTOR_MEMORY clustering.py ${@:4}
