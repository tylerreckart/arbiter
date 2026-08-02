import React from 'react';

interface ButtonProps {
  label: string;
  onClick?: () => void;
}

const Button: React.FC<ButtonProps> = ({ label, onClick }) => {
  const handleClick = () => {
    onClick?.();
  };

  return <button className="btn" onClick={handleClick}>{label}</button>;
};

export default Button;
