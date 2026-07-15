import React, { useMemo } from 'react';
import { AreaChart, Area, BarChart, Bar, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Legend } from 'recharts';
import { usePolling } from '../hooks/usePolling';
import { getMetrics } from '../services/api';
import MetricCard from '../components/MetricCard';
import LoadingSpinner from '../components/LoadingSpinner';
import ErrorAlert from '../components/ErrorAlert';

export default function MetricsPage() {
  const { data: metrics, loading, error, refresh } = usePolling(() => getMetrics(), 2000);

  const historyData = useMemo(() => {
    if (!metrics?.history) return [];
    return metrics.history.map((h, i) => ({
      idx: i,
      threads: h.active_threads || 0,
      queue: h.queue_length || 0,
      completed: h.completed || 0,
      failed: h.failed || 0,
      utilization: ((h.worker_utilization || 0) * 100).toFixed(1),
      tasks_per_sec: (h.tasks_per_sec || 0).toFixed(2),
    }));
  }, [metrics]);

  if (loading) return <LoadingSpinner text="Loading metrics..." />;
  if (error) return <ErrorAlert message={error} onRetry={refresh} />;

  return (
    <div className="space-y-6">
      <h2 className="text-2xl font-bold">Metrics</h2>

      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <MetricCard label="Submitted" value={metrics?.submitted ?? 0} color="purple" />
        <MetricCard label="Completed" value={metrics?.completed ?? 0} color="green" />
        <MetricCard label="Failed" value={metrics?.failed ?? 0} color="red" />
        <MetricCard label="Cancelled" value={metrics?.cancelled ?? 0} color="yellow" />
        <MetricCard label="Running" value={metrics?.running_tasks ?? 0} color="blue" />
        <MetricCard label="Peak Queue" value={metrics?.peak_queue_length ?? 0} color="cyan" />
        <MetricCard label="Avg Exec" value={`${(metrics?.avg_exec_time_ms ?? 0).toFixed(1)}ms`} color="green" />
        <MetricCard label="Avg Wait" value={`${(metrics?.avg_wait_time_ms ?? 0).toFixed(1)}ms`} color="yellow" />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Thread Activity & Queue Depth</h3>
          <div className="h-64">
            {historyData.length > 0 ? (
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={historyData}>
                  <CartesianGrid strokeDasharray="3 3" className="opacity-30" />
                  <XAxis dataKey="idx" hide />
                  <YAxis allowDecimals={false} fontSize={12} />
                  <Tooltip contentStyle={{ borderRadius: '8px', border: '1px solid #e5e7eb' }} />
                  <Legend />
                  <Area type="monotone" dataKey="threads" stroke="#22c55e" fill="#22c55e22" name="Threads" />
                  <Area type="monotone" dataKey="queue" stroke="#eab308" fill="#eab30822" name="Queue" />
                </AreaChart>
              </ResponsiveContainer>
            ) : (
              <div className="flex items-center justify-center h-full text-gray-400 text-sm">No history data</div>
            )}
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Throughput (Tasks/sec)</h3>
          <div className="h-64">
            {historyData.length > 0 ? (
              <ResponsiveContainer width="100%" height="100%">
                <BarChart data={historyData}>
                  <CartesianGrid strokeDasharray="3 3" className="opacity-30" />
                  <XAxis dataKey="idx" hide />
                  <YAxis fontSize={12} />
                  <Tooltip contentStyle={{ borderRadius: '8px', border: '1px solid #e5e7eb' }} />
                  <Bar dataKey="tasks_per_sec" fill="#3b82f6" name="Tasks/sec" radius={[2, 2, 0, 0]} />
                </BarChart>
              </ResponsiveContainer>
            ) : (
              <div className="flex items-center justify-center h-full text-gray-400 text-sm">No data yet</div>
            )}
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Worker Utilization (%)</h3>
          <div className="h-64">
            {historyData.length > 0 ? (
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={historyData}>
                  <CartesianGrid strokeDasharray="3 3" className="opacity-30" />
                  <XAxis dataKey="idx" hide />
                  <YAxis domain={[0, 100]} fontSize={12} />
                  <Tooltip contentStyle={{ borderRadius: '8px', border: '1px solid #e5e7eb' }} />
                  <Area type="monotone" dataKey="utilization" stroke="#8b5cf6" fill="#8b5cf622" name="Utilization %" />
                </AreaChart>
              </ResponsiveContainer>
            ) : (
              <div className="flex items-center justify-center h-full text-gray-400 text-sm">No data yet</div>
            )}
          </div>
        </div>

        <div className="bg-white dark:bg-gray-900 rounded-xl border border-gray-200 dark:border-gray-800 p-5">
          <h3 className="font-semibold mb-4">Completed vs Failed</h3>
          <div className="h-64">
            {historyData.length > 0 ? (
              <ResponsiveContainer width="100%" height="100%">
                <BarChart data={historyData}>
                  <CartesianGrid strokeDasharray="3 3" className="opacity-30" />
                  <XAxis dataKey="idx" hide />
                  <YAxis allowDecimals={false} fontSize={12} />
                  <Tooltip contentStyle={{ borderRadius: '8px', border: '1px solid #e5e7eb' }} />
                  <Legend />
                  <Bar dataKey="completed" fill="#22c55e" name="Completed" radius={[2, 2, 0, 0]} />
                  <Bar dataKey="failed" fill="#ef4444" name="Failed" radius={[2, 2, 0, 0]} />
                </BarChart>
              </ResponsiveContainer>
            ) : (
              <div className="flex items-center justify-center h-full text-gray-400 text-sm">No data yet</div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
