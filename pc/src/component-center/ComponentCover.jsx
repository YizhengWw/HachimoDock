import React from 'react';
import DeviceScreenPreview from './DeviceScreenPreview.jsx';
import './component-cover.css';

/** Live-data covers intentionally use their honest empty state, not private
 * todos/CPU information. Opening details is the explicit live-preview action. */
export default function ComponentCover({component,className=''}){
  return <div className={`component-cover ${className}`} data-cover-id={component.id} data-cover-version={component.versionHash||component.version||''}>
    <div className="component-cover__surface" inert="" aria-hidden="true">
      <DeviceScreenPreview key={`${component.id}:${component.versionHash||component.version||''}`} component={component} cover />
    </div>
  </div>;
}
