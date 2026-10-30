import Header from './components/Header.jsx';
import Hero from './components/Hero.jsx';
import Features from './components/Features.jsx';
import InTheApp from './components/InTheApp.jsx';
import Cooldown from './components/Cooldown.jsx';
import GetStarted from './components/GetStarted.jsx';
import Footer from './components/Footer.jsx';
import { useTheme } from './hooks/useTheme.js';

export default function App() {
  const { theme, toggle } = useTheme();

  return (
    <>
      <a className="skip-link" href="#main">Skip to content</a>
      <Header theme={theme} onToggleTheme={toggle} />
      <main id="main" tabIndex={-1}>
        <Hero />
        <Features />
        <InTheApp />
        <Cooldown />
        <GetStarted />
      </main>
      <Footer />
    </>
  );
}
