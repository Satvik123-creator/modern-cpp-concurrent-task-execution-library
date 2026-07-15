import React, { useState, useEffect } from 'react';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { ThemeProvider } from './context/ThemeContext';
import Layout from './components/Layout';
import DashboardPage from './pages/DashboardPage';
import WorkersPage from './pages/WorkersPage';
import QueuePage from './pages/QueuePage';
import SchedulerPage from './pages/SchedulerPage';
import MetricsPage from './pages/MetricsPage';
import TaskHistoryPage from './pages/TaskHistoryPage';
import HealthPage from './pages/HealthPage';
import { getHealth } from './services/api';

function App() {
  const [health, setHealth] = useState(null);

  useEffect(() => {
    const check = async () => {
      try {
        const res = await getHealth();
        setHealth(res.data);
      } catch {
        setHealth(null);
      }
    };
    check();
    const id = setInterval(check, 5000);
    return () => clearInterval(id);
  }, []);

  return (
    <ThemeProvider>
      <BrowserRouter>
        <Routes>
          <Route element={<Layout health={health} />}>
            <Route path="/" element={<DashboardPage />} />
            <Route path="/workers" element={<WorkersPage />} />
            <Route path="/queue" element={<QueuePage />} />
            <Route path="/scheduler" element={<SchedulerPage />} />
            <Route path="/metrics" element={<MetricsPage />} />
            <Route path="/tasks" element={<TaskHistoryPage />} />
            <Route path="/health" element={<HealthPage />} />
          </Route>
        </Routes>
      </BrowserRouter>
    </ThemeProvider>
  );
}

export default App;
