import React, { useMemo } from 'react';
import { Link } from 'react-router-dom';
import { AreaChart, Area, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';
import { usePolling } from '../hooks/usePolling';
import { getMetrics, getHealth, getWorkers, getQueue } from '../services/api';
import MetricCard from '../components/MetricCard';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';

export default function DashboardPage() {
  const { data: metrics, loading: ml, error: me, refresh: mr } = usePolling(() => getMetrics(), 2000);
  const { data: health } = usePolling(() => getHealth(), 5000);
  const { data: workers } = usePolling(() => getWorkers(), 3000);
  const { data: queue } = usePolling(() => getQueue(), 2000);

  const chartData = useMemo(() => {
    if (!metrics?.history) return [];
    return metrics.history.map((h, i) => ({
      t: i,
      threads: h.active_threads || 0,
      queue: h.queue_length || 0,
      completed: h.completed || 0,
      tasks_per_sec: h.tasks_per_sec || 0,
    }));
  }, [metrics]);

  if (ml) return <LoadingSpinner text="Loading dashboard..." />;
  if (me) return <ErrorAlert message={me} onRetry={mr} />;

  return (
    <div className="space-y-6">
      <h2 className="text-2xl font-bold">Dashboard</h2>

      <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-4">
        <MetricCard label="Active Threads" value={metrics?.active_threads ?? 0} color="green" />
        <MetricCard label="Queue Length" value={metrics?.queue_length ?? queue?.size ?? 0} color="yellow" />
        <MetricCard label="Completed" value={metrics?.completed ?? 0} color="blue" />
        <MetricCard label="Failed" value={metrics?.failed ?? 0} color="red" />
        <MetricCard label="Tasks/sec" value={(metrics?.tasks_per_sec ?? 0).toFixed(1)} color="cyan" />
        <MetricCard label="Utilization" value={`${((metrics?.worker_utilization ?? 0) * 100).toFixed(0)}%`} color="purple" />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <div className="lg:col-span-2 bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Activity Timeline</h3>
          <div className="h-64">
            {chartData.length > 0 ? (
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={chartData}>
                  <CartesianGrid strokeDasharray="3 3" className="opacity-30" />
                  <XAxis dataKey="t" hide />
                  <YAxis allowDecimals={false} fontSize={12} />
                  <Tooltip
                    contentStyle={{ borderRadius: '8px', border: '1px solid #e5e7eb' }}
                  />
                  <Area type="monotone" dataKey="threads" stroke="#22c55e" fill="#22c55e22" name="Threads" />
                  <Area type="monotone" dataKey="queue" stroke="#eab308" fill="#eab30822" name="Queue" />
                </AreaChart>
              </ResponsiveContainer>
            ) : (
              <div className="flex items-center justify-center h-full text-gray-400 text-sm">No data yet</div>
            )}
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Quick Status</h3>
          <div className="space-y-3">
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Pool</span>
              <span className={`text-sm font-medium ${health?.pool_running ? 'text-green-500' : 'text-red-500'}`}>
                {health?.pool_running ? 'Running' : 'Stopped'}
              </span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Scheduler</span>
              <span className={`text-sm font-medium ${health?.scheduler_running ? 'text-green-500' : 'text-red-500'}`}>
                {health?.scheduler_running ? 'Running' : 'Stopped'}
              </span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Workers</span>
              <span className="text-sm font-medium">{workers?.workers?.length ?? 0}</span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Queue Size</span>
              <span className="text-sm font-medium">{queue?.size ?? 0} / {queue?.capacity ?? '--'}</span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Avg Exec Time</span>
              <span className="text-sm font-medium">{(metrics?.avg_exec_time_ms ?? 0).toFixed(1)}ms</span>
            </div>
            <div className="flex justify-between items-center">
              <span className="text-sm text-gray-500">Avg Wait Time</span>
              <span className="text-sm font-medium">{(metrics?.avg_wait_time_ms ?? 0).toFixed(1)}ms</span>
            </div>
          </div>
          <div className="mt-4 pt-3 border-t border-gray-200 dark:border-gray-700">
            <Link to="/health" className="text-sm text-blue-500 hover:text-blue-400">View details →</Link>
          </div>
        </div>
      </div>
    </div>
  );
}
