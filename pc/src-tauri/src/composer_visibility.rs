//! Pure policy for read-only AX hit-test evidence. Never clicks or guesses.
pub type Bounds = (i64, i64, i64, i64);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exposure { Exposed, Covered, Unknown }

pub fn may_auto_focus(frontmost: bool, active_window_matches: bool, foreign_owner: bool, focused_role: Option<&str>) -> bool {
    frontmost && active_window_matches && !foreign_owner
        && !matches!(focused_role, Some("AXTextArea" | "AXTextField" | "AXSearchField" | "AXComboBox" | "AXSecureTextField"))
}

pub fn probe_points(control: Option<Bounds>, window: Option<Bounds>) -> Vec<(f32, f32)> {
    let (Some((x,y,w,h)), Some((wx,wy,ww,wh))) = (control, window) else { return vec![]; };
    if w <= 0 || h <= 0 || ww <= 0 || wh <= 0 { return vec![]; }
    let (left, top) = (x.max(wx), y.max(wy));
    let (right, bottom) = (x.saturating_add(w).min(wx.saturating_add(ww)),
        y.saturating_add(h).min(wy.saturating_add(wh)));
    if right <= left || bottom <= top { return vec![]; }
    let (left, top, right, bottom) = (left as f64, top as f64, right as f64, bottom as f64);
    [0.33,0.67].into_iter().flat_map(|fy| [0.25,0.5,0.75].into_iter()
        .map(move |fx| ((left+(right-left)*fx) as f32, (top+(bottom-top)*fy) as f32))).collect()
}

pub fn exposure(hits: &[Option<bool>]) -> Exposure {
    if hits.contains(&Some(true)) { Exposure::Exposed }
    else if !hits.is_empty() && hits.iter().all(|hit| *hit == Some(false)) { Exposure::Covered }
    else { Exposure::Unknown }
}

pub fn sole_exposed(states: &[Exposure]) -> Option<usize> {
    if states.contains(&Exposure::Unknown) { return None; }
    let mut visible = states.iter().enumerate().filter(|(_, s)| **s == Exposure::Exposed);
    let (index, _) = visible.next()?;
    if visible.next().is_some() { None } else { Some(index) }
}

/// A question card and the chat composer are sibling rich-text controls in
/// the same bounded composer stack. Side-panel editors are not chat inputs.
/// This fallback requires all of that evidence, not simply the lowest field.
pub fn stacked_chat_composer(bounds:&[Option<Bounds>], rich:&[bool], exposures:&[Exposure],
    related:&[Vec<bool>], window:Option<Bounds>)->Option<usize>{
    let edges=|(x,y,w,h):Bounds|->Option<(i64,i64)>{
        if w<=0||h<=0{return None;}
        Some((x.checked_add(w)?,y.checked_add(h)?))
    };
    let (_,edge)=edges(window?)?;
    if bounds.len()!=rich.len()||bounds.len()!=exposures.len()||related.len()!=bounds.len(){return None;}
    // max_by_key chose the last equal bottom; retain that ordering without
    // saturating invalid geometry into a selectable candidate.
    let mut best=None;
    for i in 0..bounds.len(){
        if rich[i]&&exposures[i]==Exposure::Exposed{
            let (_,bottom)=edges(bounds[i]?)?;
            if best.map_or(true,|(_,previous)|bottom>=previous){best=Some((i,bottom));}
        }
    }
    let (chosen,bottom)=best?;
    let (x,y,w,_)=bounds[chosen]?;let (right,_)=edges(bounds[chosen]?)?;
    if w<120||bottom>edge||edge.checked_sub(bottom)?>96{return None;}
    for i in 0..bounds.len(){
        if i==chosen||exposures[i]==Exposure::Covered{continue;}
        if exposures[i]==Exposure::Unknown{return None;}
        let (ox,_,ow,_)=bounds[i]?;
        let (other_right,other_bottom)=edges(bounds[i]?)?;
        let overlap=right.min(other_right).checked_sub(x.max(ox))?;
        if rich[i]{
            if !related[chosen].get(i).copied().unwrap_or(false)||other_bottom>y
                ||overlap<w.min(ow)/2{return None;}
        }else if overlap>0{return None;}
    }
    Some(chosen)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]fn question_card_and_side_editor_do_not_hide_main_chat_composer(){
        let b=[Some((414,596,641,20)),Some((425,700,443,36)),Some((1255,92,579,664))];
        let r=[true,true,false];let e=[Exposure::Exposed;3];
        let links=vec![vec![true,true,false],vec![true,true,false],vec![false,false,true]];
        assert_eq!(stacked_chat_composer(&b,&r,&e,&links,Some((0,0,1865,760))),Some(1));
        let independent=vec![vec![true,false,false],vec![false,true,false],vec![false,false,true]];
        assert_eq!(stacked_chat_composer(&b,&r,&e,&independent,Some((0,0,1865,760))),None);
        assert_eq!(stacked_chat_composer(&b,&[true,true,true],&e,&links,Some((0,0,1865,760))),None);
        assert_eq!(stacked_chat_composer(&b,&r,&[Exposure::Unknown,Exposure::Exposed,Exposure::Exposed],&links,Some((0,0,1865,760))),None);
        assert_eq!(stacked_chat_composer(&b,&r,&e,&links,Some((0,0,1865,960))),None);
        let reversed=[b[2],b[1],b[0]];let links=vec![vec![true,false,false],vec![false,true,true],vec![false,true,true]];
        assert_eq!(stacked_chat_composer(&reversed,&[false,true,true],&e,&links,Some((0,0,1865,760))),Some(1));
    }
    #[test] fn missing_focus_button_or_background_all_allow_automatic_focus() {
        for role in [None,Some("AXButton"),Some("AXGroup"),Some("AXWindow"),Some("AXWebArea")] {
            assert!(may_auto_focus(true,true,false,role));
        }
    }
    #[test] fn automatic_focus_does_not_cross_windows_or_steal_from_search() {
        assert!(!may_auto_focus(false,true,false,None));
        assert!(!may_auto_focus(true,false,false,None));
        assert!(!may_auto_focus(true,true,true,Some("AXButton")));
        for role in ["AXTextArea","AXTextField","AXSearchField","AXComboBox","AXSecureTextField"] {
            assert!(!may_auto_focus(true,true,false,Some(role)));
        }
    }
    #[test] fn button_focus_one_real_editor_and_one_covered_node() {
        assert_eq!(sole_exposed(&[Exposure::Covered, Exposure::Exposed]), Some(1));
        assert_eq!(sole_exposed(&[Exposure::Exposed, Exposure::Covered]), Some(0));
    }
    #[test] fn multiple_visible_editors_are_never_guessed() {
        assert_eq!(sole_exposed(&[Exposure::Exposed, Exposure::Exposed]), None);
        assert_eq!(sole_exposed(&[Exposure::Covered, Exposure::Covered]), None);
        assert_eq!(sole_exposed(&[]), None);
    }
    #[test] fn unknown_ax_is_not_treated_as_hidden() {
        assert_eq!(sole_exposed(&[Exposure::Exposed, Exposure::Unknown]), None);
        assert_eq!(exposure(&[Some(false),None]), Exposure::Unknown);
        assert_eq!(exposure(&[]), Exposure::Unknown);
    }
    #[test] fn one_proven_hit_is_enough_for_partially_occluded_editor() {
        assert_eq!(exposure(&[Some(false),Some(true),None]), Exposure::Exposed);
        assert_eq!(exposure(&[Some(false);6]), Exposure::Covered);
    }
    #[test] fn probes_are_clipped_and_allow_negative_display_coordinates() {
        let points = probe_points(Some((-700,400,400,200)),Some((-640,0,640,480)));
        assert_eq!(points.len(),6);
        for (x,y) in points { assert!(x > -640. && x < -300. && y > 400. && y < 480.); }
    }
    #[test] fn absent_zero_or_off_window_geometry_does_not_supply_evidence() {
        for rect in [None, Some((0,0,0,40)),Some((1000,0,40,40))] {
            assert!(probe_points(rect,Some((0,0,640,480))).is_empty());
        }
        assert!(probe_points(Some((0,0,100,100)),None).is_empty());
    }
    #[test] fn selection_is_order_independent_and_requires_all_other_nodes_covered() {
        for size in 2..12 {
            for selected in 0..size {
                let mut states=vec![Exposure::Covered;size];
                states[selected]=Exposure::Exposed;
                assert_eq!(sole_exposed(&states),Some(selected));
                states[(selected+1)%size]=Exposure::Unknown;
                assert_eq!(sole_exposed(&states),None);
            }
        }
    }
}


#[cfg(test)]
mod checked_geometry_tests {
    use super::*;
    fn one(bound:Bounds,window:Bounds)->Option<usize>{
        stacked_chat_composer(&[Some(bound)],&[true],&[Exposure::Exposed],&[vec![true]],Some(window))
    }
    #[test]fn overflow_gap_must_be_unknown(){
        // Both y+h endpoints individually fit; edge-bottom does not.
        assert_eq!(one((0,i64::MIN+10,200,20),(0,i64::MAX-100,640,100)),None);
    }
    #[test]fn overflowing_window_or_candidate_edges_are_unknown(){
        assert_eq!(one((0,0,200,20),(0,i64::MAX-5,640,20)),None);
        assert_eq!(one((0,0,200,20),(i64::MAX-5,0,640,20)),None);
        assert_eq!(one((i64::MAX-5,0,200,20),(0,0,640,20)),None);
        assert_eq!(one((0,i64::MAX-5,200,20),(0,i64::MAX-100,640,100)),None);
    }
    #[test]fn other_edges_and_overlap_distance_are_checked(){
        let links=vec![vec![true,false],vec![false,true]];
        for other in [(i64::MAX-5,0,20,20),(300,i64::MAX-5,20,20)]{
            assert_eq!(stacked_chat_composer(&[Some((0,100,200,20)),Some(other)],&[true,false],&[Exposure::Exposed;2],&links,Some((0,0,640,120))),None);
        }
        assert_eq!(stacked_chat_composer(&[Some((i64::MIN+10,100,200,20)),Some((i64::MAX-100,0,20,20))],&[true,false],&[Exposure::Exposed;2],&links,Some((0,0,640,120))),None);
    }
    #[test]fn nonpositive_relevant_dimensions_never_select(){
        for (w,h) in [(0,20),(-1,20),(200,0),(200,-1)]{
            assert_eq!(one((0,0,w,h),(0,0,640,20)),None);
            assert_eq!(one((0,0,200,20),(0,0,w,h)),None);
            assert_eq!(stacked_chat_composer(&[Some((0,100,200,20)),Some((300,0,w,h))],&[true,false],&[Exposure::Exposed;2],&[vec![true,false],vec![false,true]],Some((0,0,640,120))),None);
        }
    }
    #[test]fn valid_negative_and_extreme_display_coordinates_still_select(){
        let b=[Some((-286,-104,641,20)),Some((-275,0,443,36)),Some((555,-608,579,664))];
        let links=vec![vec![true,true,false],vec![true,true,false],vec![false,false,true]];
        assert_eq!(stacked_chat_composer(&b,&[true,true,false],&[Exposure::Exposed;3],&links,Some((-700,-700,1865,760))),Some(1));
        assert_eq!(one((i64::MIN+100,i64::MIN+60,200,20),(i64::MIN+1,i64::MIN+1,640,100)),Some(0));
        assert_eq!(one((i64::MAX-300,i64::MAX-100,200,80),(i64::MAX-500,i64::MAX-200,500,200)),Some(0));
    }
    #[test]fn covered_irrelevant_nodes_and_existing_ambiguity_policy_remain(){
        assert_eq!(stacked_chat_composer(&[Some((0,100,200,20)),Some((i64::MAX,0,-1,0))],&[true,false],&[Exposure::Exposed,Exposure::Covered],&[vec![true,false],vec![false,true]],Some((0,0,640,120))),Some(0));
        assert_eq!(stacked_chat_composer(&[Some((0,100,200,20))],&[],&[Exposure::Exposed],&[vec![true]],Some((0,0,640,120))),None);
    }
}
